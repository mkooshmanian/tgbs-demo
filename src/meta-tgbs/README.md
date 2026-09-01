# meta-tgbs

Reusable Yocto/OpenEmbedded integration layer for TGBS.

## Machines

* `qemuarm32`: QEMU ARM `virt` with PL011 and built-in VirtIO devices.
* `zybo-z7`: Digilent Zybo Z7 (Zynq-7000), using the upstream
  `xilinx/zynq-zybo-z7.dtb` and U-Boot's `xilinx_zynq_virt_defconfig`.

Both kernel configurations start from `allnoconfig`; common TGBS, PREEMPT_RT
and optional debug fragments are merged afterward. Programmable-logic IP for
the Zybo Z7 should be enabled in a separate kernel fragment and described by a
matching device-tree overlay.
