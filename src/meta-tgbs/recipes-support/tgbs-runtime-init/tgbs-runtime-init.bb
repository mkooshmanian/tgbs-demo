SUMMARY = "Minimal runtime cgroup v2 setup for TGBS"
DESCRIPTION = "SysV init script that mounts cgroup v2 at /sys/fs/cgroup and enables the cpu and cpuset controllers at the cgroup root, so that TGBS can create and place task groups. No subgroup is created and no temporal contract is modified."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://tgbs;beginline=1;endline=4;md5=391b3c3c8ef54feb728dabfc62a76739"

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
