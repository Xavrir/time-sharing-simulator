/*
 * tsim - a time-sharing system simulator.
 *
 * The parent process plays the part of the kernel. Forked children play the
 * part of user processes. An interval timer preempts whoever is running, and
 * the switch itself is carried out with SIGSTOP and SIGCONT, so the real
 * operating system performs every context switch.
 *
 * Read top to bottom: what a user process does, then the ready queue, then
 * the scheduler, then the timer plumbing, then main.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_PROCS 8
#define MAX_TICKS 4096

#define CPU_PRIMES 650000

#define IO_ROUNDS  26
#define IO_BURST   9000000L
#define IO_WAIT_MS 50

#define UI_ROUNDS  26
#define UI_BURST   1500000L
#define UI_WAIT_MS 130

#define CHART_WRAP 80
#define CHART_MAX  240
#define ROW_LABEL  "  P%d %-3s "
#define RULER_PAD  "         "

typedef enum { P_READY, P_RUNNING, P_DONE } proc_state;

typedef enum { W_CPU, W_IO, W_INTERACTIVE } workload;

typedef struct {
    int        id;
    pid_t      pid;
    proc_state state;
    workload   kind;

    int arrival;
    int first_run;
    int finish;
    int quanta;
    int preempted;
} pcb;

typedef struct {
    pcb procs[MAX_PROCS];
    int nprocs;

    int ready[MAX_PROCS + 1];
    int head, tail, count;

    int running;
    int tick;
    int switches;

    int quantum_ms;
    int max_ticks;
    int quiet;

    double wall_ms;
    double child_cpu_ms;
    double sched_cpu_ms;

    char chart[MAX_PROCS][MAX_TICKS];
} sim;

static volatile sig_atomic_t tick_pending;
static volatile sig_atomic_t quit_requested;
static volatile long         sink;

static void die(const char *what)
{
    perror(what);
    exit(1);
}

static const char *worker_name(workload kind)
{
    switch (kind) {
    case W_CPU:         return "cpu";
    case W_IO:          return "io";
    case W_INTERACTIVE: return "ui";
    }
    return "?";
}

/*
 * Everything from here to worker_run executes in a child, after fork. It is a
 * different process from the scheduler and shares nothing with it.
 */

static int is_prime(long n)
{
    if (n < 2)
        return 0;
    for (long d = 2; d * d <= n; d++)
        if (n % d == 0)
            return 0;
    return 1;
}

static void burn(long iterations)
{
    long acc = 0;

    for (long i = 0; i < iterations; i++)
        acc += i % 7;
    sink = acc;
}

static void nap(long ms)
{
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;

    /* SIGCONT after a preemption interrupts the sleep. nanosleep writes the
       unslept remainder back into ts, so looping resumes rather than restarts. */
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        continue;
}

static void run_cpu(int id)
{
    long found = 0;

    for (long n = 2; found < CPU_PRIMES; n++) {
        if (!is_prime(n))
            continue;
        found++;
        if (found % 50000 == 0)
            printf("    [P%d cpu] %ld primes found, now at %ld\n", id, found, n);
    }
    printf("    [P%d cpu] finished\n", id);
}

static void run_io(int id)
{
    for (int round = 1; round <= IO_ROUNDS; round++) {
        burn(IO_BURST);
        printf("    [P%d io ] disk read %d issued\n", id, round);
        nap(IO_WAIT_MS);
        printf("    [P%d io ] disk read %d returned\n", id, round);
    }
    printf("    [P%d io ] finished\n", id);
}

static void run_interactive(int id)
{
    for (int k = 1; k <= UI_ROUNDS; k++) {
        burn(UI_BURST);
        printf("    [P%d ui ] keystroke %d handled\n", id, k);
        nap(UI_WAIT_MS);
    }
    printf("    [P%d ui ] finished\n", id);
}

static void worker_run(workload kind, int id)
{
    switch (kind) {
    case W_CPU:         run_cpu(id);         break;
    case W_IO:          run_io(id);          break;
    case W_INTERACTIVE: run_interactive(id); break;
    }
}

/* The ready queue. Fixed size, so there is no allocation anywhere. */

static void queue_push(sim *s, int idx)
{
    s->ready[s->tail] = idx;
    s->tail = (s->tail + 1) % (MAX_PROCS + 1);
    s->count++;
}

static int queue_pop(sim *s)
{
    int idx;

    if (s->count == 0)
        return -1;
    idx = s->ready[s->head];
    s->head = (s->head + 1) % (MAX_PROCS + 1);
    s->count--;
    return idx;
}

/*
 * The scheduler. Round robin: the policy is simply that the queue is FIFO, so
 * queue_pop is the whole of the decision and sched_dispatch is the mechanism.
 */

static int index_of_pid(const sim *s, pid_t pid)
{
    for (int i = 0; i < s->nprocs; i++)
        if (s->procs[i].pid == pid)
            return i;
    return -1;
}

static void mark_done(sim *s, int idx)
{
    s->procs[idx].state  = P_DONE;
    s->procs[idx].finish = s->tick + 1;
    s->procs[idx].pid    = -1;
    if (s->running == idx)
        s->running = -1;
}

static void sched_record(sim *s)
{
    if (s->tick >= MAX_TICKS)
        return;

    for (int i = 0; i < s->nprocs; i++) {
        char c;

        if (s->procs[i].state == P_DONE)
            c = ' ';
        else if (i == s->running)
            c = '#';
        else
            c = '.';
        s->chart[i][s->tick] = c;
    }
}

static void sched_reap(sim *s)
{
    for (;;) {
        pid_t pid;
        int status, idx;

        pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            break;
        idx = index_of_pid(s, pid);
        if (idx >= 0)
            mark_done(s, idx);
    }
}

static void sched_preempt(sim *s)
{
    pcb *p;
    pid_t r;
    int idx, status;

    if (s->running < 0)
        return;

    idx = s->running;
    p   = &s->procs[idx];
    p->quanta++;

    /* SIGSTOP cannot be caught, blocked or ignored, which is exactly why it
       is the right instrument for preemption: the process gets no say. */
    if (kill(p->pid, SIGSTOP) < 0) {
        mark_done(s, idx);
        return;
    }

    /* Never assume the signal landed. WUNTRACED makes waitpid report the
       stop, and tells us apart the case where the child exited instead. */
    do {
        r = waitpid(p->pid, &status, WUNTRACED);
    } while (r < 0 && errno == EINTR);

    if (r < 0 || !WIFSTOPPED(status)) {
        mark_done(s, idx);
        return;
    }

    p->preempted++;
    p->state   = P_READY;
    s->running = -1;
    queue_push(s, idx);
}

static void sched_dispatch(sim *s)
{
    pcb *p;
    int idx = queue_pop(s);

    if (idx < 0)
        return;

    p = &s->procs[idx];
    if (p->first_run < 0)
        p->first_run = s->tick;
    p->state   = P_RUNNING;
    s->running = idx;
    s->switches++;
    kill(p->pid, SIGCONT);
}

static int sched_alive(const sim *s)
{
    for (int i = 0; i < s->nprocs; i++)
        if (s->procs[i].state != P_DONE)
            return 1;
    return 0;
}

static void sched_killall(sim *s)
{
    for (int i = 0; i < s->nprocs; i++) {
        if (s->procs[i].pid <= 0)
            continue;
        kill(s->procs[i].pid, SIGKILL);
        waitpid(s->procs[i].pid, NULL, 0);
        s->procs[i].pid = -1;
    }
}

/* Reporting. */

static void report_tick(const sim *s)
{
    if (s->quiet)
        return;

    printf("[tick %3d] cpu=", s->tick);
    if (s->running >= 0)
        printf("P%d", s->procs[s->running].id);
    else
        printf("--");

    printf("  ready=");
    if (s->count == 0) {
        printf("(empty)");
    } else {
        int i = s->head;

        for (int k = 0; k < s->count; k++) {
            printf("P%d ", s->procs[s->ready[i]].id);
            i = (i + 1) % (MAX_PROCS + 1);
        }
    }
    printf("\n");
}

static void print_timeline(const sim *s)
{
    int shown = s->tick > CHART_MAX ? CHART_MAX : s->tick;

    printf("\n=== execution timeline ===\n");
    printf("legend:  #  holding the CPU    .  ready and waiting    blank  finished\n\n");

    for (int start = 0; start < shown; start += CHART_WRAP) {
        int end = start + CHART_WRAP;

        if (end > shown)
            end = shown;

        printf(RULER_PAD);
        for (int t = start; t < end; t++)
            printf("%d", (t / 10) % 10);
        printf("\n" RULER_PAD);
        for (int t = start; t < end; t++)
            printf("%d", t % 10);
        printf("\n");

        for (int i = 0; i < s->nprocs; i++) {
            printf(ROW_LABEL, s->procs[i].id, worker_name(s->procs[i].kind));
            for (int t = start; t < end; t++)
                putchar(s->chart[i][t]);
            printf("\n");
        }
        printf("\n");
    }

    if (shown < s->tick)
        printf("  (chart truncated after %d quanta, %d more not shown)\n\n",
               shown, s->tick - shown);
}

static void print_stats(const sim *s)
{
    double turn_sum = 0, wait_sum = 0, resp_sum = 0;

    printf("=== statistics (all times in quanta) ===\n\n");
    printf("  id  kind  arrive  first  finish   cpu  turnaround  waiting"
           "  response  preempted\n");

    for (int i = 0; i < s->nprocs; i++) {
        const pcb *p = &s->procs[i];
        int turnaround = p->finish - p->arrival;
        int waiting    = turnaround - p->quanta;
        int response   = p->first_run - p->arrival;

        turn_sum += turnaround;
        wait_sum += waiting;
        resp_sum += response;

        printf("  P%-2d %-4s  %6d %6d  %6d  %4d  %10d  %7d  %8d  %9d\n",
               p->id, worker_name(p->kind), p->arrival, p->first_run,
               p->finish, p->quanta, turnaround, waiting, response,
               p->preempted);
    }

    printf("\n  averages in quanta:  turnaround %.2f   waiting %.2f"
           "   response %.2f\n",
           turn_sum / s->nprocs, wait_sum / s->nprocs, resp_sum / s->nprocs);
    printf("  averages in ms:      turnaround %.0f   waiting %.0f"
           "   response %.0f\n",
           turn_sum / s->nprocs * s->quantum_ms,
           wait_sum / s->nprocs * s->quantum_ms,
           resp_sum / s->nprocs * s->quantum_ms);
    printf("  turnaround = finish - arrive, waiting = turnaround - cpu,"
           " response = first - arrive\n");

    printf("\n=== cost of switching ===\n\n");
    printf("  quantum               %d ms\n", s->quantum_ms);
    printf("  context switches      %d over %d quanta\n", s->switches, s->tick);
    printf("  wall clock elapsed    %.0f ms\n", s->wall_ms);
    printf("  cpu time to workers   %.0f ms\n", s->child_cpu_ms);
    printf("  cpu time to scheduler %.0f ms\n", s->sched_cpu_ms);
    if (s->switches > 0)
        printf("  scheduler cost each   %.3f ms per switch\n",
               s->sched_cpu_ms / s->switches);
    if (s->wall_ms > 0) {
        printf("  worker share          %.1f%% of wall clock\n",
               100.0 * s->child_cpu_ms / s->wall_ms);
        printf("  scheduler share       %.2f%% of wall clock\n",
               100.0 * s->sched_cpu_ms / s->wall_ms);
    }
}

/* Signals, the timer, and process creation. */

/* Handlers do nothing but raise a flag. Anything else, printf above all,
   is not async-signal-safe and would be undefined behaviour here. */
static void on_alarm(int sig)
{
    (void)sig;
    tick_pending = 1;
}

static void on_interrupt(int sig)
{
    (void)sig;
    quit_requested = 1;
}

static void install_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    /* sigaction rather than signal: signal's behaviour around handler reset
       and syscall restart is implementation defined. */
    sa.sa_handler = on_alarm;
    if (sigaction(SIGALRM, &sa, NULL) < 0)
        die("sigaction SIGALRM");

    sa.sa_handler = on_interrupt;
    if (sigaction(SIGINT, &sa, NULL) < 0)
        die("sigaction SIGINT");
}

static void set_timer(int quantum_ms)
{
    struct itimerval it;

    it.it_value.tv_sec  = quantum_ms / 1000;
    it.it_value.tv_usec = (quantum_ms % 1000) * 1000;
    it.it_interval      = it.it_value;
    if (setitimer(ITIMER_REAL, &it, NULL) < 0)
        die("setitimer");
}

static void stop_timer(void)
{
    struct itimerval off;

    memset(&off, 0, sizeof off);
    setitimer(ITIMER_REAL, &off, NULL);
}

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* Children are reaped by the time this runs, so the kernel has already
   folded their CPU usage into RUSAGE_CHILDREN. */
static double cpu_ms(int who)
{
    struct rusage ru;

    if (getrusage(who, &ru) < 0)
        return 0.0;
    return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0
         + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
}

static void spawn(sim *s, int id, workload kind)
{
    pcb *p;
    pid_t pid;
    int idx, status;

    idx = s->nprocs++;
    p   = &s->procs[idx];

    p->id        = id;
    p->kind      = kind;
    p->arrival   = 0;
    p->first_run = -1;
    p->finish    = -1;
    p->quanta    = 0;
    p->preempted = 0;

    fflush(stdout);
    pid = fork();
    if (pid < 0)
        die("fork");

    if (pid == 0) {
        setvbuf(stdout, NULL, _IONBF, 0);
        signal(SIGALRM, SIG_DFL);
        signal(SIGINT, SIG_DFL);

        /* Stop ourselves at once so no child runs before the scheduler says
           so. Without this the workers race ahead of the first dispatch. */
        raise(SIGSTOP);
        worker_run(kind, id);
        _exit(0);
    }

    p->pid = pid;
    while (waitpid(pid, &status, WUNTRACED) < 0 && errno == EINTR)
        continue;

    p->state = P_READY;
    queue_push(s, idx);
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [-q QUANTUM_MS] [-t MAX_TICKS] [-s]\n"
            "  -q  length of one time slice in milliseconds (default 200)\n"
            "  -t  stop after this many time slices (default 120)\n"
            "  -s  silent: no per-quantum log, for clean overhead timing\n",
            prog);
    exit(2);
}

int main(int argc, char **argv)
{
    sigset_t alarm_only, resume_mask;
    double started;
    sim s;

    /* Unbuffered: the whole point of the demo is seeing parent and child
       output interleave, and block buffering hides exactly that. */
    setvbuf(stdout, NULL, _IONBF, 0);

    memset(&s, 0, sizeof s);
    s.quantum_ms = 200;
    s.max_ticks  = 120;
    s.running    = -1;
    memset(s.chart, ' ', sizeof s.chart);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-q") && i + 1 < argc)
            s.quantum_ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc)
            s.max_ticks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s"))
            s.quiet = 1;
        else
            usage(argv[0]);
    }

    if (s.quantum_ms < 1 || s.quantum_ms > 5000) {
        fprintf(stderr, "quantum must be between 1 and 5000 ms\n");
        return 2;
    }
    if (s.max_ticks < 1 || s.max_ticks > MAX_TICKS) {
        fprintf(stderr, "max ticks must be between 1 and %d\n", MAX_TICKS);
        return 2;
    }

    printf("round robin time sharing, quantum %d ms, up to %d quanta\n\n",
           s.quantum_ms, s.max_ticks);

    spawn(&s, 1, W_CPU);
    spawn(&s, 2, W_IO);
    spawn(&s, 3, W_INTERACTIVE);

    install_handlers();

    sigemptyset(&alarm_only);
    sigaddset(&alarm_only, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &alarm_only, &resume_mask) < 0)
        die("sigprocmask");

    started = now_ms();
    set_timer(s.quantum_ms);
    sched_dispatch(&s);
    report_tick(&s);

    while (!quit_requested && s.tick < s.max_ticks && sched_alive(&s)) {
        /* sigsuspend unblocks SIGALRM and waits atomically. Testing the flag
           and then calling pause() would lose a tick that arrives between
           the two. */
        while (!tick_pending && !quit_requested)
            sigsuspend(&resume_mask);

        if (quit_requested)
            break;
        tick_pending = 0;

        /* SIGALRM stays blocked for the rest of the loop body, so a tick
           cannot arrive while the ready queue is half rebuilt. */
        sched_record(&s);
        sched_reap(&s);
        sched_preempt(&s);
        s.tick++;
        if (!sched_alive(&s))
            break;
        sched_dispatch(&s);
        report_tick(&s);
    }

    stop_timer();
    sched_killall(&s);
    s.wall_ms      = now_ms() - started;
    s.child_cpu_ms = cpu_ms(RUSAGE_CHILDREN);
    s.sched_cpu_ms = cpu_ms(RUSAGE_SELF);
    sigprocmask(SIG_SETMASK, &resume_mask, NULL);

    for (int i = 0; i < s.nprocs; i++)
        if (s.procs[i].finish < 0)
            s.procs[i].finish = s.tick;

    if (quit_requested)
        printf("\ninterrupted, stopping early\n");

    print_timeline(&s);
    print_stats(&s);
    return 0;
}
