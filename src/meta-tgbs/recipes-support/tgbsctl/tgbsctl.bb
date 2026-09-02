SUMMARY = "Daemonless runtime launcher for TGBS cgroups"
DESCRIPTION = "tgbsctl creates a cgroup directly under /sys/fs/cgroup, configures the cpu period and runtime budget, then execs a command inside it using the fork/SIGSTOP/cgroup.procs/SIGCONT pattern so no task runs outside the budget. The cgroup lifetime is tied to the main process."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://tgbsctl.c file://tgbsctl.h file://observe.c"

S = "${WORKDIR}"

CFLAGS:append = " -Wall -Wextra"

do_compile() {
	${CC} ${CFLAGS} ${LDFLAGS} tgbsctl.c observe.c -o tgbsctl
}

do_install() {
	install -d ${D}${bindir}
	install -m 0755 tgbsctl ${D}${bindir}
}
