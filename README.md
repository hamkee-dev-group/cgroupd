# cgroupd

`cgroupd` is a small host-local job runner for Linux. It gives you a thin,
direct way to run processes inside cgroup v2, apply hard resource limits, and
react to real kernel pressure signals before the host falls over.

It is intentionally boring: one daemon, one CLI, one Unix socket, no image
runtime, no cluster scheduler, no YAML control plane. The daemon owns a cgroup
subtree, starts jobs inside child cgroups, watches PSI and cgroup event files,
and takes simple pressure-driven actions such as admission rejection, freezing,
CPU demotion, and killing the lowest-priority job.

This is useful when a single machine needs to run mixed workloads without
turning every workload into a container platform problem: AI inference boxes,
batch workers, edge hosts, build machines, database sidecars, and other places
where the Linux kernel already has the isolation features you want.

## What it does

- Runs jobs under a dedicated cgroup v2 subtree.
- Applies CPU, memory, IO, PID, cpuset, swap, and priority controls.
- Starts children with `clone3(CLONE_INTO_CGROUP)` when available, with a
  `fork` plus `cgroup.procs` fallback for older kernels.
- Watches `memory.events` so cgroup OOM kills are surfaced through `inspect`
  and `wait`.
- Reads PSI for memory, CPU, and IO pressure.
- Rejects new jobs when the host is already under too much pressure.
- Freezes, thaws, demotes, or kills jobs based on pressure and priority.
- Captures per-job logs when the daemon is started with a log directory.
- Supports simple preflight path checks and per-job sidecar services.
- Emits a stable key/value event stream on stderr.

## Build

Requirements:

- Linux with cgroup v2 mounted at `/sys/fs/cgroup`
- `gcc`
- `make`
- A writable delegated cgroup subtree, or root

Kernel notes:

- `cgroup.kill` needs Linux 5.14 or newer.
- `CLONE_INTO_CGROUP` needs Linux 5.7 or newer.
- Older kernels can still run jobs through the fallback attach path, but some
  newer behavior depends on the kernel features above.

```sh
make all
```

This builds:

- `build/cgroupd`
- `build/cgroupctl`
- `build/memhog`
- `build/cpuhog`

Install to `/usr/local/bin`:

```sh
sudo make install
```

Use `PREFIX` if you want a different install root:

```sh
make PREFIX="$HOME/.local" install
```

## Quick start

Start the daemon:

```sh
sudo build/cgroupd -d -s /tmp/cgroupd.sock -L /tmp/cgroupd-logs
```

Point the CLI at that socket:

```sh
export CGROUPD_SOCKET=/tmp/cgroupd.sock
```

Run a small job with memory and CPU controls:

```sh
sudo -E build/cgroupctl run \
  --id hello \
  --memory-max 64M \
  --cpu-weight 100 \
  --priority 50 \
  -- /bin/sh -c 'echo hello from cgroupd'
```

Inspect it:

```sh
sudo -E build/cgroupctl inspect hello
```

Wait for its result:

```sh
sudo -E build/cgroupctl wait hello
```

Read captured output:

```sh
sudo -E build/cgroupctl logs hello
```

Shut the daemon down:

```sh
sudo -E build/cgroupctl quit
```

## CLI

Daemon:

```text
cgroupd [options]
  -r, --root PATH                 cgroup root to own
  -s, --socket PATH               Unix socket path
  -d, --debug                     debug logging
  -k, --mem-kill-avg10 X          kill threshold for memory.full.avg10
      --mem-admit-some-avg10 X    reject jobs at memory.some.avg10
      --mem-admit-full-avg10 X    reject jobs at memory.full.avg10
      --cpu-admit-some-avg10 X    reject jobs at cpu.some.avg10
      --io-admit-full-avg10 X     reject jobs at io.full.avg10
  -L, --log-dir PATH              capture job stdout/stderr
```

Client:

```text
cgroupctl [--socket PATH] <subcommand> [args]

Subcommands:
  run --id ID [opts] -- argv...   submit a job
  list                            list jobs
  inspect ID                      per-job stats and pressure
  stats                           host pressure summary
  freeze ID                       freeze a job cgroup
  thaw ID                         thaw a job cgroup
  kill ID [signal N]              send a signal, default SIGKILL
  wait ID                         block until the job exits
  logs ID [--follow]              print captured stdout/stderr
  remove ID                       remove an exited job cgroup
  ping                            health check
  quit                            ask daemon to shut down
```

Useful `run` options:

```text
--cpu-max Q/P
--cpu-weight N
--memory-max SZ
--memory-high SZ
--memory-low SZ
--memory-min SZ
--memory-swap-max SZ
--io-weight N
--io-max RULE
--pids-max N
--cpuset-cpus CPUS
--cpuset-mems MEMS
--priority N
--cwd DIR
--env K=V
--require-path PATH
--service CMD
```

Priority is `0..100`; lower-priority jobs are picked first when pressure forces
an eviction or freeze.

## Examples

Run a CPU-limited command:

```sh
sudo -E build/cgroupctl run \
  --id cpu-demo \
  --cpu-max 50000/100000 \
  --cpu-weight 100 \
  -- /bin/sh -c 'while :; do :; done'
```

Run with a hard memory cap:

```sh
sudo -E build/cgroupctl run \
  --id mem-demo \
  --memory-max 128M \
  --memory-swap-max 0 \
  -- build/memhog 96 20
```

Start a sidecar in the same job cgroup before the main process:

```sh
sudo -E build/cgroupctl run \
  --id service-demo \
  --service "echo sidecar-ready; trap 'exit 0' TERM INT; while :; do sleep 1; done" \
  -- /bin/sh -c 'sleep 3; echo main-done'
```

Reject launch if a required path is missing:

```sh
sudo -E build/cgroupctl run \
  --id guarded \
  --require-path /srv/model.bin \
  -- /usr/local/bin/inference-worker
```

## Pressure behavior

`cgroupd` uses PSI as a feedback signal, not as a vague metric for dashboards.
The current policy is deliberately simple:

- Admission thresholds can reject `RUN` before a child cgroup is created.
- High memory `full.avg10` kills a low-priority running job.
- High memory `some.avg10` can freeze a low-priority job.
- High CPU `some.avg10` can reduce CPU weight for low-priority jobs.
- High IO `full.avg10` can freeze an IO-heavy low-priority job.
- When memory pressure recedes, frozen jobs can be thawed.

Actions are throttled so the daemon does not flap faster than PSI windows can
meaningfully update.

## Events

The daemon writes structured events to stderr:

```text
EVENT schema=cgroupd.v1 ts_unix_ms=<epoch-ms> type=<event-type> ...
```

Events are plain key/value lines so they work with shell tools, journald, or a
small supervisor without another serialization layer. See
[`docs/events.md`](docs/events.md) for the event schema.

## Tests and benchmarks

Run the test suite:

```sh
make test
```

The suite covers smoke behavior, wait semantics, admission rejection,
orchestration features, cleanup, cgroup OOM handling, and pressure-driven
eviction. Some tests need root because they set caps on the parent cgroup
slice.

Run the benchmark script:

```sh
make bench
```

It measures basic job spawn latency, spawn throughput, and reaction time under
memory pressure.

## Repository layout

```text
src/
  cgroupd.c     daemon event loop and pressure policy
  cgroupctl.c   CLI client
  cgroup.c      cgroup v2 helpers
  psi.c         PSI parsing and trigger setup
  proto.c       line-oriented socket protocol
  util.c        file IO, parsing, mkdir, clocks
  log.c         leveled stderr logging

tests/
  smoke.sh
  wait.sh
  admission.sh
  orchestration.sh
  cleanup.sh
  oom.sh
  pressure.sh
  run_bench.sh

bench/
  memhog.c
  cpuhog.c

docs/
  events.md
```
