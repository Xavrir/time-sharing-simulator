/*
 * tsim - a time-sharing system simulator.
 *
 * The parent process plays the part of the kernel. Forked children play the
 * part of user processes. An interval timer preempts whoever is running, and
 * the switch is carried out with SIGSTOP and SIGCONT, so the real operating
 * system performs every context switch.
 *
 * All three processes start together, so a process's turnaround time is simply
 * the quantum it finished on.
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

/* One entry per process: a round is some computation, then an optional wait on
   a simulated device. Varying these three shapes is what makes the cost of
   treating every process identically visible in the final report. */
static const struct {
    const char *name;
    long burst;     /* loop iterations per round, about 1.4 ns each */
    long wait_ms;   /* simulated device wait, 0 means never sleeps */
    int  rounds;
} JOBS[NPROCS] = {
    { "cpu", 125000000L,   0, 12 },
    { "io",    9000000L,  50, 26 },
    { "ui",    1500000L, 130, 26 },
};

typedef struct {
    int   id;        /* the P1, P2, P3 shown in the output */
    pid_t pid;
    int   done;
    int   first_run;
    int   finish;
    int   quanta;
} pcb;

typedef struct {
    pcb procs[NPROCS];
    int ready[NPROCS + 1];
    int head, tail, count;

    int running;
    int live;
    int tick;
    int switches;
    int quantum_ms;

    char chart[NPROCS][MAX_TICKS];
} sim;

static volatile sig_atomic_t tick_pending;
static volatile sig_atomic_t quit_requested;
/* burn's result is written here so the optimiser cannot delete the loop. */
static volatile long         sink;

static void die(const char *what)
{
    perror(what);
    exit(1);
}

/* burn, nap and worker_run execute in a child, after fork. That child is a
   separate process and shares no memory with the scheduler below. */

static void burn(long iterations)
{
    long acc = 0;

    for (long i = 0; i < iterations; i++)
        acc += i % 7;
    sink = acc;
}

static void nap(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    /* SIGCONT after a preemption interrupts the sleep. nanosleep writes the
       unslept remainder back into ts, so looping resumes rather than restarts. */
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        continue;
}

static void worker_run(int idx, int id)
{
    for (int r = 1; r <= JOBS[idx].rounds; r++) {
        burn(JOBS[idx].burst);
        printf("    [P%d %-3s] round %d of %d\n",
               id, JOBS[idx].name, r, JOBS[idx].rounds);
        if (JOBS[idx].wait_ms)
            nap(JOBS[idx].wait_ms);
    }
    printf("    [P%d %-3s] finished\n", id, JOBS[idx].name);
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
    if (s->count == 0)
        return -1;

    int idx = s->ready[s->head];

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
    s->procs[idx].done   = 1;
    s->procs[idx].finish = s->tick + 1;
    s->live--;

    /* Clear the pid as well. Once the child is reaped the kernel may hand that
       number to an unrelated process, and signalling it would be a real bug. */
    s->procs[idx].pid = -1;

    if (s->running == idx)
        s->running = -1;
}

static void sched_record(sim *s)
{
    for (int i = 0; i < NPROCS; i++) {
        char mark;

        if (s->procs[i].done)
            mark = ' ';
        else if (i == s->running)
            mark = '#';
        else
            mark = '.';

        s->chart[i][s->tick] = mark;
    }
}

static void sched_reap(sim *s)
{
    pid_t pid;
    int status;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        for (int i = 0; i < NPROCS; i++)
            if (s->procs[i].pid == pid)
                mark_done(s, i);
}

static void sched_preempt(sim *s)
{
    if (s->running < 0)
        return;

    int idx = s->running;
    pcb *p  = &s->procs[idx];

    p->quanta++;

    /* SIGSTOP cannot be caught, blocked or ignored, which is exactly why it
       is the right instrument for preemption: the process gets no say. */
    if (kill(p->pid, SIGSTOP) < 0) {
        mark_done(s, idx);
        return;
    }

    /* Never assume the signal landed. WUNTRACED makes waitpid report the stop,
       and distinguishes it from the child having exited instead. */
    int status;
    pid_t r;

    do {
        r = waitpid(p->pid, &status, WUNTRACED);
    } while (r < 0 && errno == EINTR);

    if (r < 0 || !WIFSTOPPED(status)) {
        mark_done(s, idx);
        return;
    }

    s->running = -1;
    queue_push(s, idx);
}

static void sched_dispatch(sim *s)
{
    int idx = queue_pop(s);

    if (idx < 0)
        return;

    if (s->procs[idx].first_run < 0)
        s->procs[idx].first_run = s->tick;
    s->running = idx;
    s->switches++;
    kill(s->procs[idx].pid, SIGCONT);
}

static void sched_killall(sim *s)
{
    for (int i = 0; i < NPROCS; i++) {
        if (s->procs[i].done)
            continue;
        kill(s->procs[i].pid, SIGKILL);
        waitpid(s->procs[i].pid, NULL, 0);
        s->procs[i].pid = -1;
    }
}

/* Reporting. */

static void report_tick(const sim *s)
{
    if (s->running < 0)
        printf("[tick %3d] cpu=--\n", s->tick);
    else
        printf("[tick %3d] cpu=P%d\n", s->tick, s->procs[s->running].id);
}

static void print_timeline(const sim *s)
{
    printf("\n=== execution timeline ===\n");
    printf("legend:  #  holding the CPU    .  ready and waiting    blank  finished\n");
    printf("         each column is one quantum, marked every ten\n\n");

    for (int start = 0; start < s->tick; start += CHART_WRAP) {
        int end = start + CHART_WRAP < s->tick ? start + CHART_WRAP : s->tick;

        printf("         ");
        for (int t = start; t < end; t++)
            putchar(t % 10 ? ' ' : '0' + (t / 10) % 10);

        for (int i = 0; i < NPROCS; i++) {
            printf("\n  P%d %-3s ", s->procs[i].id, JOBS[i].name);
            for (int t = start; t < end; t++)
                putchar(s->chart[i][t]);
        }
        printf("\n\n");
    }
}

static void print_stats(const sim *s)
{
    double turn = 0, wait = 0, resp = 0;

    printf("=== statistics (times in quanta) ===\n\n");
    printf("  id  kind   cpu  turnaround  waiting  response\n");

    for (int i = 0; i < NPROCS; i++) {
        const pcb *p = &s->procs[i];

        turn += p->finish;
        wait += p->finish - p->quanta;
        resp += p->first_run;
        printf("  P%-2d %-4s  %4d  %10d  %7d  %8d\n", p->id, JOBS[i].name,
               p->quanta, p->finish, p->finish - p->quanta, p->first_run);
    }

    printf("\n  average turnaround %.1f, waiting %.1f, response %.1f"
           " (response = %.0f ms)\n",
           turn / NPROCS, wait / NPROCS, resp / NPROCS,
           resp / NPROCS * s->quantum_ms);
    printf("  %d context switches over %d quanta of %d ms\n",
           s->switches, s->tick, s->quantum_ms);
}

/* Signals, the timer, and process creation. */

/* Handlers do nothing but raise a flag. Anything else, printf above all, is not
   async-signal-safe and would be undefined behaviour here. */
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

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-q QUANTUM_MS]   1 to 5000, default 200\n", prog);
    exit(2);
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

static void spawn(sim *s, int idx)
{
    s->procs[idx].id        = idx + 1;
    s->procs[idx].first_run = -1;
    s->procs[idx].finish    = -1;

    fflush(stdout);

    pid_t pid = fork();

    if (pid < 0)
        die("fork");

    if (pid == 0) {
        setvbuf(stdout, NULL, _IONBF, 0);
        signal(SIGALRM, SIG_DFL);
        signal(SIGINT, SIG_DFL);

        /* Stop ourselves at once so no child runs before the scheduler says
           so. Without this the workers race ahead of the first dispatch. */
        raise(SIGSTOP);
        worker_run(idx, s->procs[idx].id);
        _exit(0);
    }

    s->procs[idx].pid = pid;

    int status;

    while (waitpid(pid, &status, WUNTRACED) < 0 && errno == EINTR)
        continue;
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
    memset(s.chart, ' ', sizeof s.chart);
    s.quantum_ms = 200;
    s.running    = -1;
    s.live       = NPROCS;

    if (argc == 3 && !strcmp(argv[1], "-q"))
        s.quantum_ms = atoi(argv[2]);
    else if (argc != 1)
        usage(argv[0]);

    if (s.quantum_ms < 1 || s.quantum_ms > 5000)
        usage(argv[0]);

    printf("round robin time sharing, quantum %d ms\n\n", s.quantum_ms);

    for (int i = 0; i < NPROCS; i++)
        spawn(&s, i);

    install_handlers();
    sigemptyset(&alarm_only);
    sigaddset(&alarm_only, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &alarm_only, &resume_mask) < 0)
        die("sigprocmask");

    set_timer(s.quantum_ms);
    sched_dispatch(&s);
    report_tick(&s);

    while (!quit_requested && s.live > 0 && s.tick < MAX_TICKS - 1) {
        /* sigsuspend unblocks SIGALRM and waits atomically. Testing the flag
           and then calling pause() would lose a tick arriving between the two. */
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
        if (s.live == 0)
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
