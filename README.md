# tsim — a time-sharing system simulator in C

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

| Process | Behaviour |
|---|---|
| P1 `cpu` | pure computation, finds primes by trial division, never sleeps |
| P2 `io` | short burst of work, then waits on a simulated disk read |
| P3 `ui` | very short burst, then a long idle, imitating a user typing |

Round robin treats all three the same. The report at the end shows what that
costs each of them.

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

The one boundary worth keeping in mind is that `run_cpu`, `run_io`,
`run_interactive` and the helpers they call execute in a **child** process,
after `fork`. Everything else runs in the parent. They share no memory; the only
thing passing between them is signals.

Under round robin the policy is just that the queue is FIFO, so `queue_pop` is
the entire scheduling decision and `sched_dispatch` is the mechanism that acts
on it. There is no dynamic allocation anywhere.

## Reading the output

While running, one line per quantum shows which process holds the CPU and what
is queued behind it, interleaved with the workers' own output.

At the end a timeline is drawn, one row per process:

```
         000000000011111111112222222222333333333344
         012345678901234567890123456789012345678901
  P1 cpu #..#..#..#..#..#..#..#..#..#.#.#.#.#.#####
  P2 io  .#..#..#..#..#..#..#..#..#
  P3 ui  ..#..#..#..#..#..#..#..#..#.#.#.#.#.#
```

`#` means the process held the CPU during that quantum, `.` means it was ready
and waiting, blank means it had finished. No process runs to completion before
the others start, which is the property that makes this time-sharing rather than
batch processing.

Notice the spacing change. While all three compete a process gets every third
quantum, so its row reads `#..#..`. Once one finishes, the survivors get every
second quantum, `#.#.`, and the last one left runs solid. Responsiveness
improves as load drops, without any code deciding that it should.

### Metrics

All three processes start together, so times are counted in quanta from zero.

- **turnaround** is the quantum a process finished on
- **waiting** is turnaround minus the quanta it actually got
- **response** is the quantum it first reached the CPU

Response matters most for a time-sharing system, because it is the delay a user
actually feels. Halving the quantum halves it, which is the whole argument for
short time slices.

## Limitations

Deliberate, to keep the program small enough to explain in full.

- A process that blocks on simulated IO still consumes its whole quantum. A real
  scheduler would let it yield so someone else could run.
- All processes start at the same time. There is no staggered arrival.
- Round robin is the only policy.
- The CPU-bound worker needs a fixed amount of computation, so on a busy machine
  it is handed less real CPU per quantum and appears to need more of them.
  Compare runs on an otherwise idle system.
