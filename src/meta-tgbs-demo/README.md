# meta-tgbs-demo

Yocto/OpenEmbedded layer containing components specific to the TGBS demonstration.

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
