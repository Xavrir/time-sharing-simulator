#ifndef TSIM_H
#define TSIM_H

#include <sys/types.h>

#define MAX_PROCS 8
#define MAX_TICKS 4096

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

void sched_init(sim *s);
void sched_admit(sim *s, int idx);
void sched_record(sim *s);
void sched_reap(sim *s);
void sched_preempt(sim *s);
void sched_dispatch(sim *s);
int  sched_alive(const sim *s);
void sched_killall(sim *s);

void        worker_run(workload kind, int id);
const char *worker_name(workload kind);

void report_tick(const sim *s);
void report_final(const sim *s);

#endif
