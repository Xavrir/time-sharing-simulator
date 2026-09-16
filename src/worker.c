#include <errno.h>
#include <stdio.h>
#include <time.h>

#include "tsim.h"

#define CPU_PRIMES 220000

#define IO_ROUNDS   26
#define IO_BURST    9000000L
#define IO_WAIT_MS  50

#define UI_ROUNDS   26
#define UI_BURST    1500000L
#define UI_WAIT_MS  130

static volatile long sink;

const char *worker_name(workload kind)
{
    switch (kind) {
    case W_CPU:         return "cpu";
    case W_IO:          return "io";
    case W_INTERACTIVE: return "ui";
    }
    return "?";
}

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
        if (found % 20000 == 0)
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

void worker_run(workload kind, int id)
{
    switch (kind) {
    case W_CPU:         run_cpu(id);         break;
    case W_IO:          run_io(id);          break;
    case W_INTERACTIVE: run_interactive(id); break;
    }
}
