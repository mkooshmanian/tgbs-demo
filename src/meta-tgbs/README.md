# meta-tgbs

Reusable Yocto/OpenEmbedded integration layer for TGBS.

## Machines

* `qemuarm32`: QEMU ARM `virt` with PL011 and built-in VirtIO devices.
* `zybo-z7`: Digilent Zybo Z7 (Zynq-7000), using the upstream
  `xilinx/zynq-zybo-z7.dtb` and U-Boot's `xilinx_zynq_virt_defconfig`.

Both kernel configurations start from a machine-specific minimal defconfig;
common TGBS, PREEMPT_RT and optional debug fragments are merged afterward, then
`olddefconfig` fills unspecified symbols from their Kconfig defaults.
Machine defconfigs live in `recipes-kernel/linux/files/machines/`, while
reusable feature fragments live in `recipes-kernel/linux/files/fragments/`.
Programmable-logic IP for the Zybo Z7 should be enabled in a separate kernel
fragment and described by a matching device-tree overlay.

## Runtime control

The image integration mounts a unified cgroup v2 hierarchy and enables both
the `cpu` and `cpuset` controllers. `tgbsctl` creates one direct child of the
cgroup root per managed workload and can configure its temporal contract, CPU
placement, and optional DEADLINE bandwidth reclaim:

```sh
tgbsctl run --name worker --runtime-us 20000 --period-us 100000 \
    --cpus 0 --reclaim false /usr/bin/worker

tgbsctl set worker cpus 0-1
tgbsctl set worker cpus inherit
tgbsctl set worker reclaim true
```

CPU placement uses the standard `cpuset.cpus` CPU-list syntax. `inherit`
restores the cgroup root's current effective CPU list; this works for populated
domains, for which the kernel may reject an empty `cpuset.cpus`. Boolean reclaim
values accept `0`, `1`, `false`, and `true`.

`run --cpus` applies the CPU placement before the non-zero temporal contract,
so the kernel admission test sees the intended CPUs. With TGBS 1.4, a later
`set NAME cpus ...` updates the active per-CPU servers but does not rerun the
bandwidth admission test transactionally. Dynamic placement changes must
therefore keep the sum of overlapping reservations within the configured
DEADLINE bandwidth limit.

## Runtime monitor

`tgbs-top` is a lightweight interactive CPU monitor limited to domains managed
by `tgbsctl` and to the tasks inside those domains:

```sh
tgbs-top
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
tgbs-top --batch --iterations 3 --delay 0.5
```
