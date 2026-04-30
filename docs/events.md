# cgroupd Event Schema

`cgroupd` emits a stable line-oriented event stream to stderr. Each event is a
single line:

```text
EVENT schema=cgroupd.v1 ts_unix_ms=<epoch-ms> type=<event-type> ...
```

Current event types:

- `job.start`: `id`, `pid`, `priority`, `cgroup`
- `job.freeze`: `id`, `reason`, `cgroup`
- `job.thaw`: `id`, `reason`, `cgroup`
- `job.kill`: `id`, `signal`, `reason`, `cgroup`
- `job.oom`: `id`, `cgroup`, `oom_kill_count`
- `job.throttle`: `id`, `resource`, `cpu_weight`, `reason`, `cgroup`
- `job.exit`: `id`, `state`, `exit`, `signal`, `oom_killed`, `cgroup`,
  `start_unix_ms`, `exit_unix_ms`
- `job.cleanup`: `id`, `result`, `cgroup`
- `pressure`: `resource`, `some_avg10`, `full_avg10`, `action`, `target`
  Current actions include `kill`, `freeze`, `demote`, and `admission_reject`.

The event payload is intentionally key/value only so it can be consumed by
shell tooling, journald, or a lightweight orchestrator without an additional
serialization layer.
