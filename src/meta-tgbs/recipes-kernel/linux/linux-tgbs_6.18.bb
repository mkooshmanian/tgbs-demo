SUMMARY = "Linux 6.18 kernel with TGBS"
DESCRIPTION = "Vanilla Linux 6.18 kernel patched with Task Group Bandwidth Server (TGBS)."
HOMEPAGE = "https://github.com/mkooshmanian/TGBS"

LICENSE = "GPL-2.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=6bc538ed5bd9a7fc9398086aedcd7e46"

inherit kernel

LINUX_VERSION = "6.18"
TGBS_VERSION = "1.4"

PV = "${LINUX_VERSION}+tgbs${TGBS_VERSION}"

SRC_URI = " \
    https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${LINUX_VERSION}.tar.xz;name=kernel \
    https://github.com/mkooshmanian/TGBS/archive/refs/tags/patch/tgbs-v${TGBS_VERSION}-k${LINUX_VERSION}.tar.gz;name=tgbs \
    file://tgbs.cfg \
"

SRC_URI += "${@bb.utils.contains('DISTRO_FEATURES', 'preempt-rt', \
    'https://cdn.kernel.org/pub/linux/kernel/projects/rt/6.18/older/patch-6.18.13-rt4.patch.xz;name=preempt-rt file://preempt-rt.cfg', \
    '', d)}"

SRC_URI[kernel.sha256sum] = "9106a4605da9e31ff17659d958782b815f9591ab308d03b0ee21aad6c7dced4b"
SRC_URI[tgbs.sha256sum] = "86e94f7896559ca588a9db00dcd59da32a5639fa4e51da395aae87d7e3f082a1"
SRC_URI[preempt-rt.sha256sum] = "df2a7c6a03eb7795a6cf73645c4e0461017222d22318fed38f34edfcc1842043"

S = "${WORKDIR}/linux-${LINUX_VERSION}"
B = "${WORKDIR}/build"

TGBS_PATCH_DIR = "${WORKDIR}/TGBS-patch-tgbs-v${TGBS_VERSION}-k${LINUX_VERSION}/patches"

# qemuarm is a 32-bit ARMv7 Cortex-A15 machine using QEMU's "virt" board.
# multi_v7_defconfig provides the required ARCH_VIRT, PL011 and VirtIO support.
KBUILD_DEFCONFIG:qemuarm = "multi_v7_defconfig"
COMPATIBLE_MACHINE = "^qemuarm$"

TGBS_CONFIG_FRAGMENTS = "${WORKDIR}/tgbs.cfg"
TGBS_CONFIG_FRAGMENTS:append = "${@bb.utils.contains('DISTRO_FEATURES', \
    'preempt-rt', ' ${WORKDIR}/preempt-rt.cfg', '', d)}"

do_apply_tgbs_patches() {
    bbnote "Applying TGBS ${TGBS_VERSION} patch series"

    for patch in ${TGBS_PATCH_DIR}/*.patch; do
        bbnote "Applying $(basename ${patch})"
        patch -d ${S} -p1 < ${patch}
    done
}

addtask apply_tgbs_patches after do_patch before do_configure

do_configure:prepend() {
    oe_runmake -C ${S} O=${B} ${KBUILD_DEFCONFIG}
    ${S}/scripts/kconfig/merge_config.sh -m -O ${B} \
        ${B}/.config ${TGBS_CONFIG_FRAGMENTS}
}

do_configure:append() {
    if ! grep -q '^CONFIG_TG_BANDWIDTH_SERVER=y$' ${B}/.config; then
        bbfatal "TGBS could not be enabled; check the dependencies in tgbs.cfg"
    fi

    if ${@bb.utils.contains('DISTRO_FEATURES', 'preempt-rt', 'true', 'false', d)} \
       && ! grep -q '^CONFIG_PREEMPT_RT=y$' ${B}/.config; then
        bbfatal "PREEMPT_RT could not be enabled; check preempt-rt.cfg and the RT patch"
    fi
}

PROVIDES += "virtual/kernel"
