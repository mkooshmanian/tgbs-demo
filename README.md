# TGBS Demo

Yocto-based demonstration for TGBS (Task Group Bandwidth Server).

## Targets

Currently supported:

* QEMU ARM (`qemuarm32`)
* Digilent Zybo Z7 / Zynq-7000 (`zybo-z7`)

The QEMU configuration uses a 32-bit ARM (ARMv7) target with 2 virtual CPUs and 1 GiB of RAM to approximate the main hardware characteristics of the Zynq-7000 SoC used on the Zybo Z7.

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

## License

This project is licensed under the MIT License. See `LICENSE`.
