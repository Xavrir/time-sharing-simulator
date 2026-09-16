# tsim — a time-sharing system simulator in C

A preemptive round-robin scheduler built on real UNIX processes. The parent
process plays the part of the kernel; forked children play the part of user
processes. Preemption is driven by an interval timer and carried out with
signals, so the context switches are performed by the operating system rather
than simulated in software.

Written for COMP6697001 Operating Systems (BINUS), AOL Case Study.

## Requirements

Linux (or another UNIX), GCC, and nothing else. No external libraries.

## Build and run

```
make
./tsim
```

Useful variations:

```
./tsim -q 50          # shorter time slice
./tsim -q 400         # longer time slice
./tsim -q 10 -s       # quiet, for clean overhead measurement
make debug            # AddressSanitizer + UndefinedBehaviorSanitizer build
```

| Flag | Meaning | Default |
|---|---|---|
| `-q MS` | length of one time slice | 200 ms |
| `-t N` | stop after N time slices | 120 |
| `-s` | suppress the per-quantum log | off |

## What it simulates

Three workers with deliberately different personalities, so that the effect of
scheduling them identically becomes visible:

| Process | Behaviour |
|---|---|
| P1 `cpu` | pure computation, finds primes by trial division, never sleeps |
| P2 `io` | short burst of work, then waits on a simulated disk read |
| P3 `ui` | very short burst, then a long idle, imitating a user typing |

Round robin gives all three the same treatment. That is the point: the report
at the end shows what that costs the interactive process.

## How it works

### Mechanism

1. Each child calls `raise(SIGSTOP)` on itself immediately after `fork`, so no
   worker runs before the scheduler dispatches it. The parent confirms the stop
   with `waitpid(..., WUNTRACED)` before starting the clock.
2. The parent installs a `SIGALRM` handler with `sigaction` and arms a repeating
   timer with `setitimer(ITIMER_REAL, ...)`, where `it_interval` is the quantum.
3. The handler does one thing: set a `volatile sig_atomic_t` flag. Nothing else
   is legal there, because only async-signal-safe functions may be called from a
   signal handler.
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

### Structure

| File | Responsibility |
|---|---|
| `src/main.c` | argument parsing, signal and timer setup, the main loop |
| `src/scheduler.c` | ready queue, policy, and the stop/resume mechanism |
| `src/worker.c` | the three workloads the children run |
| `src/report.c` | the live per-quantum log, the timeline, the statistics |
| `src/tsim.h` | the process control block and shared declarations |

Choosing the next process is kept separate from performing the switch. A
different policy, such as a multi-level feedback queue, would replace only the
choice and leave the mechanism untouched.

There is no dynamic allocation anywhere. The process table and the ready queue
are fixed-size arrays.

## Reading the output

While running, one line per quantum shows which process holds the CPU and what
is queued behind it, interleaved with the workers' own output.

At the end, a timeline is drawn with one row per process:

```
         000000000011111111112222222222333
         012345678901234567890123456789012
  P1 cpu #..#..#..#..#..#..#..#..#
  P2 io  .#..#..#..#..#..#..#..#..#
  P3 ui  ..#..#..#..#..#..#..#..#..#######
```

`#` means the process held the CPU during that quantum, `.` means it was ready
and waiting, and a blank means it had finished. No process runs to completion
before the others start, which is the property that makes this time-sharing
rather than batch processing.

### Metrics

All per-process times are counted in quanta.

- **turnaround** = finish − arrive
- **waiting** = turnaround − cpu
- **response** = first dispatch − arrive

Response time matters most for a time-sharing system, because it is the delay a
user actually feels.

## Measured behaviour

Same workload, same machine, varying only the quantum:

| Quantum | Switches | Avg response | Scheduler share of wall clock |
|---:|---:|---:|---:|
| 200 ms | 30 | 200 ms | 0.05 % |
| 50 ms | 92 | 50 ms | 0.13 % |
| 10 ms | 420 | 10 ms | 0.48 % |
| 2 ms | 2106 | 2 ms | 1.82 % |

Shortening the quantum improves responsiveness in direct proportion, and the
price is paid in scheduling overhead, which grows by a factor of roughly thirty
across this range. On Linux the absolute cost of a `SIGSTOP`/`SIGCONT` pair is
small enough that overhead never dominates here; what the numbers show is the
trend, not a collapse.

## Known limitations

These are deliberate, to keep the program small enough to be explained in full.

- A process that blocks on simulated IO still consumes its whole quantum. A real
  scheduler would let it yield and dispatch someone else immediately.
- All processes arrive at the same time. There is no staggered arrival.
- Round robin is the only policy.
- A stopped child's `nanosleep` deadline keeps running while it is stopped, so
  simulated IO overlaps with other processes' execution. That happens to
  resemble real asynchronous IO, but it is a side effect rather than a design.
