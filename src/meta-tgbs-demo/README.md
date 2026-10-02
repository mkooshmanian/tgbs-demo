# meta-tgbs-demo

Yocto/OpenEmbedded layer containing components specific to the TGBS demonstration.

## System summary

`tgbs-fetch` is a lightweight, dependency-free equivalent to `fastfetch`
tailored to the demo image:

```sh
tgbs-fetch
```

It reports the host, distribution, kernel, architecture, CPU, memory, uptime,
TGBS availability, and number of active TGBS domains. Use `--no-color` or set
`NO_COLOR` for plain output.

## Interactive container

`tgbs-demo-self` opens an interactive shell as PID 1 in an isolated PID,
mount, UTS, and IPC namespace set and in the `self` TGBS domain:

```sh
tgbs-demo-self start
```

The shell displays its cgroup and namespace identifiers on entry. Type `exit`
or press Ctrl-D to leave it; `tgbsctl` then removes the domain.

## Communication channels

`tgbs-demo-comm` creates three domains, two queuing channels and one sampling
channel:

```text
comm-a --comm-a-to-b--> comm-b
comm-a <--comm-b-to-a-- comm-b
comm-c --comm-c-to-ab--> comm-a, comm-b  (sampling)
```

The initiator sends one ping per second and the responder returns a pong.
`comm-c` publishes a numbered sample every second. Both `comm-a` and `comm-b`
read the latest sample every 500 ms independently of their queuing exchanges,
and log its `VALID` or `INVALID` freshness. The refresh period is 2 seconds.
All three domains run in the background and write separately to
`/var/log/tgbs-demo/comm-a.log`, `comm-b.log` and `comm-c.log`:

```sh
tgbs-demo-comm start
tgbs-demo-comm logs
tgbs-demo-comm stop
```

`logs` follows all three files with `tail -f`. The `stop` command stops all
three domains before deleting their channels.

To check expiry without consuming or deleting the sample, freeze the producer:

```sh
tgbsctl pause comm-c
# After 2 seconds, both subscribers report INVALID for the last sample.
tgbsctl resume comm-c
# Publication resumes and both subscribers report VALID again.
```

## Mixed RT timeline

`tgbs-demo-mixed-timeline` displays the response time of every periodic RT task
started by `tgbs-demo-mixed`. The collector and workload may be started in
either order:

```sh
# Timeline terminal
tgbs-demo-mixed-timeline

# Control terminal
tgbs-demo-mixed start
```

Use `tgbs-demo-mixed pause [CONFIG]` and `tgbs-demo-mixed resume [CONFIG]`
to freeze and unfreeze the configured domain. The Doom demo supports
`tgbs-demo-doom pause` and `tgbs-demo-doom resume` in the same way. These
commands use the cgroup freezer through `tgbsctl`.

Each task has its own scrolling Braille line plot and automatic vertical scale.
All plots share the same wall-clock window, so their horizontal positions
represent the same instants even though the tasks have different periods.
The default window is 30 seconds and can be changed with `--window SEC`.
Braille cells provide sub-character resolution, and the plot height adapts to
the available terminal space. The summary shows statistics for the visible
window and the cumulative deadline-miss count. Samples that exceed the task's
period (its implicit deadline) are red; the deadline remains visible as `D` in
the task header. Press `q` to quit.

The timeline and mixed entrypoint exchange the existing fixed-size `FAKEJOB`
datagrams over the abstract Unix socket `@tgbs-demo-mixed`. Only `TaskRT`
workloads publish to this socket. The response time is reconstructed as:

```text
finish - (T0 + iteration * period)
```
