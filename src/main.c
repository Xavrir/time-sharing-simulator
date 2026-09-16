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

#include "tsim.h"

static volatile sig_atomic_t tick_pending;
static volatile sig_atomic_t quit_requested;

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

static void die(const char *what)
{
    perror(what);
    exit(1);
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

static void stop_timer(void)
{
    struct itimerval off;

    memset(&off, 0, sizeof off);
    setitimer(ITIMER_REAL, &off, NULL);
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
    sched_admit(s, idx);
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

    sched_init(&s);

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

    report_final(&s);
    return 0;
}
