#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "tsim.h"

void sched_init(sim *s)
{
    s->head = s->tail = s->count = 0;
    s->running  = -1;
    s->tick     = 0;
    s->switches = 0;
    memset(s->chart, ' ', sizeof s->chart);
}

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

void sched_admit(sim *s, int idx)
{
    s->procs[idx].state = P_READY;
    queue_push(s, idx);
}

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

void sched_record(sim *s)
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

void sched_reap(sim *s)
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

void sched_preempt(sim *s)
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

void sched_dispatch(sim *s)
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

int sched_alive(const sim *s)
{
    for (int i = 0; i < s->nprocs; i++)
        if (s->procs[i].state != P_DONE)
            return 1;
    return 0;
}

void sched_killall(sim *s)
{
    for (int i = 0; i < s->nprocs; i++) {
        if (s->procs[i].pid <= 0)
            continue;
        kill(s->procs[i].pid, SIGKILL);
        waitpid(s->procs[i].pid, NULL, 0);
        s->procs[i].pid = -1;
    }
}
