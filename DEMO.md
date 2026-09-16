# Video shot list

Target length 5 to 10 minutes. Run everything from a clean terminal.

## 1. The machine (about 1 minute)

Show the system the program runs on.

```bash
uname -a && lsb_release -d 2>/dev/null; gcc --version | head -1
```

## 2. Build from source (about 30 seconds)

```bash
make clean && make
```

Point out that it compiles with `-Wall -Wextra -Werror` and produces no
warnings, and that it needs no libraries beyond libc.

## 3. The default run (about 2 minutes)

```bash
./tsim
```

While it runs, point at the interleaving: the workers' output lines are mixed
together, and the per-quantum log shows the CPU changing hands. When it
finishes, walk through the timeline and say the key sentence out loud: no
process runs to completion before the others start.

Then read two rows off the statistics table and explain what turnaround,
waiting, and response mean.

## 4. The mechanism in the code (about 3 minutes)

Open the files in this order and explain only these points.

- `src/main.c`, the `spawn` function: why each child stops itself with
  `raise(SIGSTOP)` before doing any work.
- `src/main.c`, `on_alarm`: why the handler only sets a flag, and what would go
  wrong if it called `printf`.
- `src/main.c`, the main loop: why `sigsuspend` instead of checking the flag and
  calling `pause`, and why `SIGALRM` stays blocked for the rest of the body.
- `src/scheduler.c`, `sched_preempt`: why `SIGSTOP` is the right signal, and why
  the code waits with `WUNTRACED` instead of assuming the signal landed.
- `src/scheduler.c`, `sched_dispatch`: the three lines that are the actual
  context switch.

## 5. Changing the quantum (about 2 minutes)

Run these back to back and compare the reported average response time.

```bash
./tsim -q 400
./tsim -q 50
```

Then show the cost side, using quiet mode so the logging does not distort the
measurement.

```bash
./tsim -q 200 -s | tail -12
./tsim -q 2 -t 4000 -s | tail -12
```

Point at `scheduler share of wall clock` rising from roughly a twentieth of a
percent to nearly two percent. Say plainly that overhead does not take over the
machine at these values, and that what the numbers demonstrate is the direction
of the tradeoff.

## 6. Cleanup and correctness (about 30 seconds)

```bash
make debug && ./tsim -q 100 -t 200 -s | tail -12
```

State that it runs clean under AddressSanitizer and UndefinedBehaviorSanitizer.

Then start a run, press Ctrl+C partway through, and show that nothing is left
behind.

```bash
pgrep -a tsim || echo "no orphan processes"
```

## Points worth stating explicitly

The examiner is likely to probe these.

- `SIGSTOP` and `SIGKILL` cannot be caught, blocked, or ignored, and that is
  exactly why `SIGSTOP` suits preemption.
- Only async-signal-safe functions may be called from a signal handler.
- `sigaction` is preferred over `signal` because `signal`'s behaviour around
  handler reset and syscall restart is implementation defined.
- A stopped child cannot exit or send a signal, so the parent must resume it
  before expecting anything from it.
- The children's output is unbuffered on purpose; with normal block buffering
  the interleaving would be invisible and the demo would look broken.
