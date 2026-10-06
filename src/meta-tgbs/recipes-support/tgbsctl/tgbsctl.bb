SUMMARY = "Daemonless runtime and control tool for TGBS cgroups"
DESCRIPTION = "tgbsctl manages TGBS cgroups and immutable communication channels without a daemon. Commands run as PID 1 in private namespaces over a shared static base, with persistent per-domain uppers by default or an optional ephemeral tmpfs upper. Private runtime mounts and removal of CAP_SYS_ADMIN protect the prepared mount view."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

FILESEXTRAPATHS:prepend := "${THISDIR}/../channel-common/files:"

SRC_URI = "file://tgbsctl.c file://tgbsctl.h file://run.c file://rootfs.c file://observe.c file://control.c file://channel.c file://channel-contract.c file://channel-contract.h file://queuing-format.h file://sampling-format.h"

S = "${WORKDIR}"

RDEPENDS:${PN} += "tgbs-runtime-init"

CFLAGS:append = " -Wall -Wextra"

do_compile() {
	${CC} ${CFLAGS} ${LDFLAGS} tgbsctl.c run.c rootfs.c observe.c control.c channel.c channel-contract.c -o tgbsctl
}

do_install() {
	install -d ${D}${bindir}
	install -m 0755 tgbsctl ${D}${bindir}
}
