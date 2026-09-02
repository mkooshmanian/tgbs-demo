# TGBS Demo

Yocto-based demonstration for TGBS (Task Group Bandwidth Server).

## Targets

Currently supported:

* QEMU ARM (`qemuarm32`)
* Digilent Zybo Z7 / Zynq-7000 (`zybo-z7`)

The QEMU configuration matches the Zynq-7000 SoC: an ARMv7 (Cortex-A9) target with 2 virtual CPUs and 1 GiB of RAM, sharing the same CPU tune as the Zybo Z7 so both machines build with a single toolchain.

## Project Structure

```text
.
├── kas/
│   ├── local.yml
│   ├── qemu.yml
│   ├── tgbs-demo.yml
│   └── zybo-z7.yml
└── src/
    ├── meta-openembedded/
    ├── meta-kdebug/
    ├── meta-tgbs/
    ├── meta-tgbs-demo/
    └── poky/
```

The kas configuration is split by responsibility:

* `kas/tgbs-demo.yml` — common demo and image configuration
* `kas/qemu.yml` — QEMU target configuration
* `kas/zybo-z7.yml` — Zybo Z7 target configuration
* `kas/local.yml` — local build host configuration

The Yocto layers are organized as follows:

* `meta-kdebug` — optional kernel configuration and GDB helpers
* `meta-tgbs` — reusable Yocto integration for TGBS
* `meta-tgbs-demo` — components and configuration specific to the demonstration
* `poky` and `meta-openembedded` — upstream Yocto/OpenEmbedded layers

## Build Host Configuration

The default local configuration uses shared Yocto download and sstate directories:

```text
/opt/yocto/downloads
/opt/yocto/sstate-cache
```

These paths are defined in `kas/local.yml` and may be adapted to the build host.

## Build

Build the QEMU image with:

```sh
kas build kas/tgbs-demo.yml:kas/qemu.yml:kas/local.yml
```

Build the Zybo Z7 image and its kernel device tree with:

```sh
kas build kas/tgbs-demo.yml:kas/zybo-z7.yml:kas/local.yml
```

The Zybo build deploys `boot.bin`, `u-boot.bin`, `zImage`,
`zynq-zybo-z7.dtb`, and the root filesystem. SD-card image assembly is not
automated yet.

## Run

Enter the configured Yocto build environment:

```sh
kas shell kas/tgbs-demo.yml:kas/qemu.yml:kas/local.yml
```

Then boot the image with:

```sh
runqemu qemuarm32 nographic
```

The demo image allows direct root login without a password.

## Kernel Debugging

The `meta-kdebug` layer enables the kernel debug information and GDB scripts
when the `kdebug` distribution feature is present. The default development
configuration enables it in `kas/tgbs-demo.yml`.

Install the following host tools before starting a debug session:

* `gdb-multiarch`
* OpenOCD for the Zybo Z7 target
* the Cortex-Debug VS Code extension (`marus25.cortex-debug`)

The two ready-to-use configurations are provided in `.vscode/launch.json`.
Both use Cortex-Debug, with a different server mode for each backend. QEMU
already exposes a GDB remote stub, so Cortex-Debug connects to it as an
external server. The physical target is accessed through an OpenOCD instance
started and managed by Cortex-Debug.

### QEMU

Build the QEMU image, enter its build environment and start it:

```sh
kas build kas/tgbs-demo.yml:kas/qemu.yml:kas/local.yml
kas shell kas/tgbs-demo.yml:kas/qemu.yml:kas/local.yml
runqemu qemuarm32 nographic
```

When `kdebug` is enabled, `meta-kdebug` adds `-s` to the QEMU command line.
The GDB remote stub therefore listens on TCP port 1234. The **Attach QEMU**
configuration uses Cortex-Debug with `servertype: "external"` and connects to
this endpoint through `gdbTarget`. Select it in the VS Code Run and Debug view
to attach to the running kernel.

By default, `-s` does not stop the virtual CPU and the kernel starts before GDB
connects. To debug the earliest kernel code, add the following setting to the
`kernel-debug` block in `kas/tgbs-demo.yml`, then rebuild the image:

```yaml
local_conf_header:
  kernel-debug: |
    DISTRO_FEATURES:append = " kdebug"
    KDEBUG_QEMU_ARGS = "-s -S"
```

QEMU will then wait for GDB before executing the first instruction. Continue
the target from the debugger after setting the required breakpoints.

### Zybo Z7

Build and boot the Zybo Z7 image, connect the Digilent JTAG adapter, then
select **Attach Zynq** in VS Code. The Cortex-Debug configuration starts
OpenOCD with these configuration files:

```text
interface/ftdi/digilent-hs1.cfg
target/zynq_7000.cfg
```

OpenOCD must therefore be available in `PATH`, and the current user must have
permission to access the JTAG adapter.

Cortex-Debug can also attach to an OpenOCD instance managed outside VS Code,
using the same external-server mode as the QEMU configuration. For that use
case, replace the managed OpenOCD settings with, for example:

```json
"servertype": "external",
"gdbTarget": "localhost:3333"
```

In external mode, Cortex-Debug connects to the supplied GDB endpoint and does
not start or configure OpenOCD itself.

### Linux GDB helpers

Both launch configurations load the generated `vmlinux` symbol file and
`vmlinux-gdb.py`. This Python script is produced by the Linux kernel build when
`CONFIG_GDB_SCRIPTS=y` and adds commands and convenience functions that
understand kernel data structures. The target normally needs to be stopped
while these commands inspect its memory.

Some useful commands are:

```gdb
(gdb) lx-dmesg
(gdb) lx-ps
(gdb) lx-lsmod
(gdb) lx-symbols
```

`lx-dmesg` prints the kernel log buffer directly from target memory, even when
no userspace shell or serial console is available. `lx-ps` walks the kernel
task list and displays the address, PID and name of each task. `lx-lsmod` lists
loaded modules, while `lx-symbols` loads or refreshes their symbols.

Use GDB itself to discover every helper supported by the kernel version being
debugged:

```gdb
(gdb) apropos lx
(gdb) help lx-dmesg
(gdb) help function lx_task_by_pid
```

The paths in `.vscode/launch.json` include the kernel recipe version and its
Yocto work directory. Update them if the kernel version, recipe name or target
machine changes.

## License

This project is licensed under the MIT License. See `LICENSE`.
