#include <stdio.h>

#include "tsim.h"

#define CHART_WRAP 80
#define CHART_MAX  240
#define ROW_LABEL  "  P%d %-3s "
#define RULER_PAD  "         "

void report_tick(const sim *s)
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
    printf("\n=== execution timeline ===\n");
    printf("legend:  #  holding the CPU    .  ready and waiting    blank  finished\n\n");

    int shown = s->tick > CHART_MAX ? CHART_MAX : s->tick;

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

void report_final(const sim *s)
{
    print_timeline(s);
    print_stats(s);
}
