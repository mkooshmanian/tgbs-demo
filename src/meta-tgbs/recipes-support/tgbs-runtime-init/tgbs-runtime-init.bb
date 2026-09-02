SUMMARY = "Minimal runtime cgroup v2 setup for TGBS"
DESCRIPTION = "SysV init script that mounts cgroup v2 at /sys/fs/cgroup and enables the cpu controller at the cgroup root, so that TGBS can create task groups. No subgroup is created and no budget or period is modified."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://tgbs;beginline=1;md5=5a58c71df43092d176c9d8bde2a4aaff"

SRC_URI = "file://tgbs"

S = "${WORKDIR}"

inherit update-rc.d

INITSCRIPT_NAME = "tgbs"
INITSCRIPT_PARAMS = "start 04 S . stop 99 0 6 ."

FILES:${PN} = "${sysconfdir}/init.d/tgbs"

do_install() {
	install -d ${D}${sysconfdir}/init.d
	install -m 0755 ${WORKDIR}/tgbs ${D}${sysconfdir}/init.d/tgbs
}
