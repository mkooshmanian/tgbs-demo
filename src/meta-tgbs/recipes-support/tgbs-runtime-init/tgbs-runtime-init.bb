SUMMARY = "Static rootfs overlay and runtime cgroup v2 setup for TGBS"
DESCRIPTION = "Early init mounts a writable tmpfs overlay above the read-only image and keeps the static base at /rofs. The SysV init script mounts cgroup v2, enables resource controllers, and initializes the volatile communication-channel store."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://tgbs;beginline=1;endline=4;md5=391b3c3c8ef54feb728dabfc62a76739"

SRC_URI = "file://tgbs file://tgbs-preinit"

S = "${WORKDIR}"

inherit update-rc.d

INITSCRIPT_NAME = "tgbs"
INITSCRIPT_PARAMS = "start 04 S . stop 99 0 6 ."

RDEPENDS:${PN} += "busybox"

FILES:${PN} = "${sysconfdir}/init.d/tgbs ${base_sbindir}/tgbs-preinit /rofs"

do_install() {
	install -d ${D}${sysconfdir}/init.d
	install -m 0755 ${WORKDIR}/tgbs ${D}${sysconfdir}/init.d/tgbs
	install -d ${D}${base_sbindir} ${D}/rofs
	install -m 0755 ${WORKDIR}/tgbs-preinit ${D}${base_sbindir}/tgbs-preinit
}
