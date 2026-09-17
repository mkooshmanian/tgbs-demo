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

## TGBS monitor

`tgbs-demo-top` is installed in the demo image. It is a lightweight interactive
CPU monitor limited to domains managed by `tgbsctl` and to the tasks inside
those domains:

```sh
tgbs-demo-top
```

The first section shows every logical CPU and aggregates busy time into TGBS,
other, and idle shares. Domain rows show the configured per-CPU reservation
(`cpu.runtime_us / cpu.period_us`) and a `USE/BUDGET` bar. That bar compares
measured CPU consumption with the total reservation across the effective CPU
set; a full bar means that the domain consumed its complete reserved capacity
during the latest sample. Task rows show the scheduling policy, priority,
current CPU, CPU time consumed during the latest sample (`CPU ms`), and context
switches per second (`CSW/s`). CPU percentages use the same convention as
`top`: 100% is one fully occupied logical CPU.

Press `q` to leave the interactive view. For scripts or captures, use batch
mode, for example:

```sh
tgbs-demo-top --batch --iterations 3 --delay 0.5
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
