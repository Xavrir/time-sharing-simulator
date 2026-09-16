# Video shot list

Target 5 to 10 minutes. Close other heavy programs first, since a loaded machine
changes the numbers.

## 1. The system (about 1 minute)

```bash
uname -a; gcc --version | head -1
```

## 2. Build (about 30 seconds)

```bash
make clean && make
```

Point out that it compiles with `-Wall -Wextra -Werror` and no warnings, and
needs no libraries beyond libc.

## 3. Run it (about 2 minutes)

```bash
./tsim
```

While it runs, point at the interleaving: worker output from three processes
mixed together, and the per-quantum line showing the CPU changing hands.

When it finishes, walk through the timeline and say the key sentence out loud:
no process runs to completion before the others start. Then point at the spacing
changing from `#..#..` to `#.#.` to solid as processes finish, and explain that
a process's share depends on how many others are competing.

Read the statistics table and define turnaround, waiting, and response.

## 4. The code (about 3 minutes)

One file, a few jumps.

- `spawn`: why each child stops itself with `raise(SIGSTOP)` before doing any
  work, and that everything in `worker_run` runs in a different process.
- `on_alarm`: why the handler only sets a flag, and what would go wrong if it
  called `printf`.
- The main loop: why `sigsuspend` rather than checking the flag and calling
  `pause`, and why `SIGALRM` stays blocked for the rest of the body.
- `sched_preempt`: why `SIGSTOP` is the right signal, and why the code waits
  with `WUNTRACED` instead of assuming the signal landed.
- `sched_dispatch`: the few lines that are the actual context switch, and that
  `queue_pop` above them is the entire policy under round robin.

## 5. Changing the quantum (about 1 minute)

```bash
./tsim -q 400
./tsim -q 50
```

Compare the reported average response time. It tracks the quantum directly.
Say what the cost is: every switch is work the machine does without making
progress, so shorter slices buy responsiveness and pay for it in overhead.

## 6. Cleanup (about 30 seconds)

Start a run, press Ctrl+C partway through, then:

```bash
pgrep tsim || echo "no orphan processes"
```

## Likely examiner questions

- `SIGSTOP` and `SIGKILL` cannot be caught, blocked, or ignored, which is why
  `SIGSTOP` suits preemption.
- Only async-signal-safe functions may be called from a signal handler.
- `sigaction` is preferred over `signal` because `signal`'s behaviour around
  handler reset and syscall restart is implementation defined.
- A stopped child cannot exit or send a signal, so the parent must resume it
  before expecting anything from it.
- The children's output is unbuffered on purpose; with block buffering the
  interleaving would be invisible.
