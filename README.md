# tsim a time-sharing system simulator in C

A preemptive round-robin scheduler built on real UNIX processes. The parent
process plays the part of the kernel; three forked children play the part of
user processes. Preemption is driven by an interval timer and carried out with
signals, so the context switches are performed by the operating system rather
than simulated in software.

Written for COMP6697001 Operating Systems (BINUS), AOL Case Study.

## Build and run

Needs Linux (or another UNIX) and GCC. Nothing else.

```
make
./tsim
./tsim -q 50      # shorter time slice
./tsim -q 400     # longer time slice
```

## What it simulates

Three workers with deliberately different habits, so that the effect of
scheduling them identically becomes visible.

| Process | Arrives | Behaviour |
|---|---|---|
| P1 `cpu` | quantum 0 | pure computation, never sleeps |
| P2 `io` | quantum 3 | short burst of work, then waits on a simulated disk read |
| P3 `ui` | quantum 6 | very short burst, then a long idle, imitating a user typing |

They do not all start together. Each enters the system at its own arrival
quantum, which is what a real system looks like: work shows up while other work
is already running. Before a process arrives it is forked but stopped, and holds
no place in the ready queue.

Round robin treats everything present the same. The report at the end shows what
that costs each of them.

## How it works

1. Each child calls `raise(SIGSTOP)` on itself immediately after `fork`, so no
   worker runs before the scheduler dispatches it. The parent confirms the stop
   with `waitpid(..., WUNTRACED)` before starting the clock.
2. The parent installs a `SIGALRM` handler with `sigaction` and arms a repeating
   timer with `setitimer(ITIMER_REAL, ...)`, where `it_interval` is the quantum.
3. The handler does one thing: set a `volatile sig_atomic_t` flag. Only
   async-signal-safe functions may be called from a signal handler, which rules
   out almost everything else, `printf` included.
4. The main loop waits in `sigsuspend`, which unblocks `SIGALRM` and sleeps in a
   single atomic step. Checking the flag and then calling `pause` would lose any
   tick that arrived between the two.
5. On each tick the parent stops the running child with `SIGSTOP`, moves it to
   the tail of the ready queue, takes the next child from the head, and resumes
   it with `SIGCONT`.
6. `SIGALRM` stays blocked for the whole loop body, so a tick can never arrive
   while the ready queue is half rebuilt.

`SIGSTOP` is the right instrument for preemption precisely because it cannot be
caught, blocked, or ignored. The process being preempted gets no say, which is
what separates preemptive multitasking from cooperative multitasking.

### Reading the code

Everything is in `main.c`, in reading order: what a user process does, then the
ready queue, then the scheduler, then the signal and timer plumbing, then
`main`. Every function is `static` except `main`.

The three workers differ only in numbers, so they share one function driven by
the `JOBS` table at the top: how much computation per round, how long to wait on
a simulated device afterwards, and how many rounds. Change a row there to change
a process's character.

The one boundary worth keeping in mind is that `burn`, `nap` and `worker_run`
execute in a **child** process, after `fork`. Everything else runs in the
parent. They share no memory; the only thing passing between them is signals.

Under round robin the policy is just that the queue is FIFO, so `queue_pop` is
the entire scheduling decision and `sched_dispatch` is the mechanism that acts
on it. There is no dynamic allocation anywhere.

## Reading the output

While running, one line per quantum shows which process holds the CPU and who is
queued behind it, interleaved with the workers' own output.

```
           + P2 io arrives
[tick   3] running P1   ready: P2
[tick   4] running P2   ready: P1
```

Lines beginning `+` and `-` are a process entering or leaving the system. They
are indented to sit under the tick prefix, so they read as something that
happened during that quantum rather than as a quantum of their own.

Watch the `ready:` list rather than the running process. Seeing the same
identifiers rotate through it is the clearest evidence that this is round robin
and not something picking favourites.

At the end a timeline is drawn, one row per process:

```
         0         1         2         3
  P1 cpu ####.#.#..#..#..#..#..#..#..#.#.#.#.#
  P2 io     .#.#..#..#..#..#..#..#..#
  P3 ui        ..#..#..#..#..#..#..#..#.#.#.#.###
```

`#` means the process held the CPU during that quantum, `.` means it was ready
and waiting, and blank means it was not in the system, either not yet arrived or
already finished. No process runs to completion before the others get a turn,
which is the property that makes this time-sharing rather than batch processing.

The row spacing is the whole story, and it changes four times in that one chart.

- **Quanta 0 to 3.** P1 is alone, so its row is solid. It is still preempted at
  the end of every quantum; the scheduler simply has nobody else to pick and
  hands the CPU straight back.
- **Quanta 3 to 6.** P2 arrives, and the two of them alternate: `#.#.`
- **Quanta 6 onward.** P3 arrives and each process gets every third quantum:
  `#..#..`
- **After P2 finishes.** The survivors go back to every second quantum, and the
  last one left runs solid again.

Nothing in the code decides any of that. A process's share is simply one divided
by the number of processes in the queue, and responsiveness rises and falls with
load on its own.

### Metrics

Times are counted in quanta, measured from each process's own arrival.

- **turnaround** is finish minus arrive
- **waiting** is turnaround minus the quanta it actually got
- **response** is the quantum it first reached the CPU, minus arrive

Because both burst and arrival are visible in the table, the figures can be
checked against a hand calculation rather than taken on trust.

Response matters most for a time-sharing system, because it is the delay a user
actually feels. Halving the quantum halves it, which is the whole argument for
short time slices.

A process that did not finish, which happens if you interrupt a run, shows a
dash rather than a turnaround time, and is left out of the averages. It has no
turnaround time yet, and inventing one would be a lie.

## Limitations

Deliberate, to keep the program small enough to explain in full.

- A process that blocks on simulated IO still consumes its whole quantum. A real
  scheduler would let it yield so someone else could run.
- The scheduler performs a stop and a resume at every quantum boundary, even when
  it is about to hand the CPU back to the process it just took it from. A real
  one would skip that.
- Round robin is the only policy.
- The CPU-bound worker needs a fixed amount of computation, so on a busy machine
  it is handed less real CPU per quantum and appears to need more of them.
  Compare runs on an otherwise idle system.
