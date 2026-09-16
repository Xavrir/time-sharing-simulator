/*
 * tsim - a time-sharing system simulator.
 *
 * The parent process plays the part of the kernel. Forked children play the
 * part of user processes. An interval timer preempts whoever is running, and
 * the switch is carried out with SIGSTOP and SIGCONT, so the real operating
 * system performs every context switch.
 *
 * All three processes start at the same time, so a process's turnaround time
 * is simply the quantum it finished on.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NPROCS     3
#define MAX_TICKS  400
#define CHART_WRAP 80

#define CPU_PRIMES 650000

#define IO_ROUNDS  26
#define IO_BURST   9000000L
#define IO_WAIT_MS 50

#define UI_ROUNDS  26
#define UI_BURST   1500000L
#define UI_WAIT_MS 130

typedef enum { P_READY, P_RUNNING, P_DONE } proc_state;

typedef enum { W_CPU, W_IO, W_INTERACTIVE } workload;

typedef struct {
    int        id;
    pid_t      pid;
    proc_state state;
    workload   kind;

    int first_run;
    int finish;
    int quanta;
} pcb;

typedef struct {
    pcb procs[NPROCS];

    int ready[NPROCS + 1];
    int head, tail, count;

    int running;
    int tick;
    int switches;
    int quantum_ms;

    char chart[NPROCS][MAX_TICKS];
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
 * different process from the scheduler and shares no memory with it.
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
    s->tail = (s->tail + 1) % (NPROCS + 1);
    s->count++;
}

static int queue_pop(sim *s)
{
    int idx;

    if (s->count == 0)
        return -1;
    idx = s->ready[s->head];
    s->head = (s->head + 1) % (NPROCS + 1);
    s->count--;
    return idx;
}

/*
 * The scheduler. Round robin means the queue is plain FIFO, so queue_pop above
 * is the whole of the policy and sched_dispatch below is the mechanism.
 */

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
    for (int i = 0; i < NPROCS; i++) {
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
        int status;

        pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            break;
        for (int i = 0; i < NPROCS; i++)
            if (s->procs[i].pid == pid)
                mark_done(s, i);
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
    for (int i = 0; i < NPROCS; i++)
        if (s->procs[i].state != P_DONE)
            return 1;
    return 0;
}

static void sched_killall(sim *s)
{
    for (int i = 0; i < NPROCS; i++) {
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
            i = (i + 1) % (NPROCS + 1);
        }
    }
    printf("\n");
}

static void print_timeline(const sim *s)
{
    printf("\n=== execution timeline ===\n");
    printf("legend:  #  holding the CPU    .  ready and waiting    blank  finished\n\n");

    for (int start = 0; start < s->tick; start += CHART_WRAP) {
        int end = start + CHART_WRAP;

        if (end > s->tick)
            end = s->tick;

        printf("         ");
        for (int t = start; t < end; t++)
            printf("%d", (t / 10) % 10);
        printf("\n         ");
        for (int t = start; t < end; t++)
            printf("%d", t % 10);
        printf("\n");

        for (int i = 0; i < NPROCS; i++) {
            printf("  P%d %-3s ", s->procs[i].id, worker_name(s->procs[i].kind));
            for (int t = start; t < end; t++)
                putchar(s->chart[i][t]);
            printf("\n");
        }
        printf("\n");
    }
}

static void print_stats(const sim *s)
{
    double turn_sum = 0, wait_sum = 0, resp_sum = 0;

    printf("=== statistics (all times in quanta) ===\n\n");
    printf("  id  kind   cpu  turnaround  waiting  response\n");

    for (int i = 0; i < NPROCS; i++) {
        const pcb *p = &s->procs[i];
        int waiting = p->finish - p->quanta;

        turn_sum += p->finish;
        wait_sum += waiting;
        resp_sum += p->first_run;

        printf("  P%-2d %-4s  %4d  %10d  %7d  %8d\n",
               p->id, worker_name(p->kind), p->quanta, p->finish,
               waiting, p->first_run);
    }

    printf("\n  averages in quanta:  turnaround %.2f   waiting %.2f"
           "   response %.2f\n",
           turn_sum / NPROCS, wait_sum / NPROCS, resp_sum / NPROCS);
    printf("  averages in ms:      turnaround %.0f   waiting %.0f"
           "   response %.0f\n",
           turn_sum / NPROCS * s->quantum_ms,
           wait_sum / NPROCS * s->quantum_ms,
           resp_sum / NPROCS * s->quantum_ms);
    printf("\n  turnaround = quantum it finished on, waiting = turnaround - cpu,\n");
    printf("  response = quantum it first reached the CPU\n");
    printf("  %d context switches over %d quanta of %d ms\n",
           s->switches, s->tick, s->quantum_ms);
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

static void spawn(sim *s, int idx, int id, workload kind)
{
    pcb *p = &s->procs[idx];
    pid_t pid;
    int status;

    p->id        = id;
    p->kind      = kind;
    p->first_run = -1;
    p->finish    = -1;

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

int main(int argc, char **argv)
{
    sigset_t alarm_only, resume_mask;
    sim s;

    /* Unbuffered: the whole point of the demo is seeing parent and child
       output interleave, and block buffering hides exactly that. */
    setvbuf(stdout, NULL, _IONBF, 0);

    memset(&s, 0, sizeof s);
    s.quantum_ms = 200;
    s.running    = -1;
    memset(s.chart, ' ', sizeof s.chart);

    if (argc == 3 && !strcmp(argv[1], "-q")) {
        s.quantum_ms = atoi(argv[2]);
    } else if (argc != 1) {
        fprintf(stderr, "usage: %s [-q QUANTUM_MS]   (default 200)\n", argv[0]);
        return 2;
    }

    if (s.quantum_ms < 1 || s.quantum_ms > 5000) {
        fprintf(stderr, "quantum must be between 1 and 5000 ms\n");
        return 2;
    }

    printf("round robin time sharing, quantum %d ms\n\n", s.quantum_ms);

    spawn(&s, 0, 1, W_CPU);
    spawn(&s, 1, 2, W_IO);
    spawn(&s, 2, 3, W_INTERACTIVE);

    install_handlers();

    sigemptyset(&alarm_only);
    sigaddset(&alarm_only, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &alarm_only, &resume_mask) < 0)
        die("sigprocmask");

    set_timer(s.quantum_ms);
    sched_dispatch(&s);
    report_tick(&s);

    while (!quit_requested && s.tick < MAX_TICKS - 1 && sched_alive(&s)) {
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

    set_timer(0);
    sched_killall(&s);
    sigprocmask(SIG_SETMASK, &resume_mask, NULL);

    for (int i = 0; i < NPROCS; i++)
        if (s.procs[i].finish < 0)
            s.procs[i].finish = s.tick;

    if (quit_requested)
        printf("\ninterrupted, stopping early\n");

    print_timeline(&s);
    print_stats(&s);
    return 0;
}
