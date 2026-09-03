SUMMARY = "Daemonless runtime and control tool for TGBS cgroups"
DESCRIPTION = "tgbsctl creates and observes TGBS cgroups directly under /sys/fs/cgroup. It can run a command under a temporal contract and CPU set, inspect its state, kill or freeze its processes, and update its runtime, period, CPU placement, or reclaim policy without a daemon. The cgroup lifetime is tied to the main process."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://tgbsctl.c file://tgbsctl.h file://observe.c file://control.c"

S = "${WORKDIR}"

RDEPENDS:${PN} += "tgbs-runtime-init"

CFLAGS:append = " -Wall -Wextra"

do_compile() {
	${CC} ${CFLAGS} ${LDFLAGS} tgbsctl.c observe.c control.c -o tgbsctl
}

do_install() {
	install -d ${D}${bindir}
	install -m 0755 tgbsctl ${D}${bindir}
}
