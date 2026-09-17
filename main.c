/*
 * tsim - a time sharing simulator.
 *
 * The parent acts as the kernel. The forked children are the user processes.
 * A timer cuts off whoever is running, and SIGSTOP and SIGCONT do the swap, so
 * the real OS performs every context switch rather than us faking one.
 *
 * The processes do not all start at once. Each one shows up at its own arrival
 * quantum, and its times are counted from there.
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

/* Indent boundary events under the "[tick NNN] " prefix so they look like part
   of that quantum, not a new one with the same number. */
#define EVENT_PAD  "           "

/* One row per process. A round is some work, then maybe a wait on a fake
   device. Giving them different shapes is what shows, in the final report,
   what it costs to treat them all the same. */
static const struct {
    const char *name;
    int  arrival;   /* quantum at which this process enters the system */
    long burst;     /* loop iterations per round, about 1.4 ns each */
    long wait_ms;   /* simulated device wait, 0 means never sleeps */
    int  rounds;
} JOBS[NPROCS] = {
    { "cpu", 0, 125000000L,   0, 12 },
    { "io",  3,   9000000L,  50, 26 },
    { "ui",  6,   1500000L, 130, 26 },
};

typedef struct {
    int   id;        /* the P1, P2, P3 shown in the output */
    pid_t pid;
    int   arrived;
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
/* burn writes its result here so the compiler cannot throw the loop away. */
static volatile long         sink;

static void die(const char *what)
{
    perror(what);
    exit(1);
}

/* burn, nap and worker_run run inside a child, after fork. That child is its
   own process and shares no memory with the scheduler below. */

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

    /* Being resumed cuts the sleep short. nanosleep puts the time left back
       into ts, so the loop picks up where it stopped instead of starting over. */
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
    /* No "finished" line here. The scheduler says it when it reaps the child,
       and saying it twice in a row reads badly. */
}

/* The ready queue. Fixed size, so nothing is ever allocated. */

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
 * The scheduler. Round robin just means the queue is FIFO, so queue_pop above
 * is the entire policy and sched_dispatch below is how it gets carried out.
 */

static void mark_done(sim *s, int idx)
{
    s->procs[idx].done   = 1;
    s->procs[idx].finish = s->tick + 1;
    s->live--;
    printf(EVENT_PAD "- P%d %s finishes\n", s->procs[idx].id, JOBS[idx].name);

    /* Clear the pid too. Once the child is reaped the kernel can give that
       number to some other process, and signalling that would be a real bug. */
    s->procs[idx].pid = -1;

    if (s->running == idx)
        s->running = -1;
}

static void sched_record(sim *s)
{
    for (int i = 0; i < NPROCS; i++) {
        char mark;

        if (!s->procs[i].arrived || s->procs[i].done)
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

    /* SIGSTOP cannot be caught, blocked or ignored. That is the point. The
       process gets no say in being stopped. */
    if (kill(p->pid, SIGSTOP) < 0) {
        mark_done(s, idx);
        return;
    }

    /* Do not assume the signal landed. WUNTRACED makes waitpid report the
       stop, and tells it apart from the child having exited. */
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

/* The scheduler does not know a process exists until its arrival quantum.
   Before that it is forked but stopped, and not in the ready queue. */
static void sched_admit(sim *s)
{
    for (int i = 0; i < NPROCS; i++) {
        if (JOBS[i].arrival != s->tick)
            continue;

        s->procs[i].arrived = 1;
        queue_push(s, i);
        printf(EVENT_PAD "+ P%d %s arrives\n", s->procs[i].id, JOBS[i].name);
    }
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
    printf("[tick %3d] ", s->tick);

    if (s->running < 0)
        printf("idle      ");
    else
        printf("running P%d", s->procs[s->running].id);

    /* Who is waiting matters as much as who is running. The queue rotating is
       the thing round robin actually does. */
    printf("   ready:");
    if (s->count == 0) {
        printf(" none");
    } else {
        int i = s->head;

        for (int k = 0; k < s->count; k++) {
            printf(" P%d", s->procs[s->ready[i]].id);
            i = (i + 1) % (NPROCS + 1);
        }
    }
    printf("\n");
}

static void print_timeline(const sim *s)
{
    printf("\n=== execution timeline ===\n");
    printf("legend:  #  holding the CPU    .  ready and waiting\n");
    printf("         blank  not in the system, either not yet arrived"
           " or already finished\n");
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
    int finished = 0;

    printf("=== statistics (times in quanta) ===\n\n");
    printf("  id  kind  arrive   cpu  turnaround  waiting  response\n");

    for (int i = 0; i < NPROCS; i++) {
        const pcb *p = &s->procs[i];

        printf("  P%-2d %-4s  %6d  %4d", p->id, JOBS[i].name,
               JOBS[i].arrival, p->quanta);

        /* A process that never finished has no turnaround time. Printing one
           anyway would be a lie, and if it was cut off before it even arrived
           the maths goes negative. */
        if (!p->done) {
            printf("  %10s  %7s  %8s\n", "-", "-", "-");
            continue;
        }

        int turnaround = p->finish - JOBS[i].arrival;
        int waiting    = turnaround - p->quanta;
        int response   = p->first_run - JOBS[i].arrival;

        turn += turnaround;
        wait += waiting;
        resp += response;
        finished++;
        printf("  %10d  %7d  %8d\n", turnaround, waiting, response);
    }

    printf("\n  turnaround = finish - arrive     waiting = turnaround - cpu\n");
    printf("  response   = first cpu - arrive\n");

    if (finished > 0) {
        printf("\n  averages over the %d that finished:\n", finished);
        printf("    turnaround %.1f   waiting %.1f   response %.1f (= %.0f ms)\n",
               turn / finished, wait / finished, resp / finished,
               resp / finished * s->quantum_ms);
    }
    printf("  %d context switches over %d quanta of %d ms\n",
           s->switches, s->tick, s->quantum_ms);
}

/* Signals, the timer, and process creation. */

/* Handlers only set a flag. Anything else, printf above all, is not async
   signal safe, and calling it in here is undefined behaviour. */
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

    /* sigaction, not signal. What signal does about resetting the handler and
       restarting syscalls is left up to the implementation. */
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

        /* Stop right away so no child runs before the scheduler picks it.
           Without this the workers get a head start on the first dispatch. */
        raise(SIGSTOP);
        worker_run(idx, s->procs[idx].id);
        _exit(0);
    }

    s->procs[idx].pid = pid;

    int status;

    while (waitpid(pid, &status, WUNTRACED) < 0 && errno == EINTR)
        continue;
}

int main(int argc, char **argv)
{
    sigset_t alarm_only, resume_mask;
    sim s;

    /* Unbuffered. The demo is about watching parent and child output mix
       together, and buffering hides exactly that. */
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
    sched_admit(&s);
    sched_dispatch(&s);
    report_tick(&s);

    while (!quit_requested && s.live > 0 && s.tick < MAX_TICKS - 1) {
        /* sigsuspend unblocks SIGALRM and waits in one step. Checking the flag
           and then calling pause() would drop a tick landing between the two. */
        while (!tick_pending && !quit_requested)
            sigsuspend(&resume_mask);
        if (quit_requested)
            break;
        tick_pending = 0;

        /* SIGALRM stays blocked for the rest of the loop, so a tick cannot
           land while the ready queue is half rebuilt. */
        sched_record(&s);
        sched_reap(&s);
        sched_preempt(&s);
        s.tick++;
        if (s.live == 0)
            break;
        sched_admit(&s);
        sched_dispatch(&s);
        report_tick(&s);
    }

    set_timer(0);
    sched_killall(&s);
    sigprocmask(SIG_SETMASK, &resume_mask, NULL);

    if (quit_requested)
        printf("\ninterrupted, stopping early\n");

    print_timeline(&s);
    print_stats(&s);
    return 0;
}
