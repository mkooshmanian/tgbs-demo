SUMMARY = "Static rootfs overlay and runtime cgroup v2 setup for TGBS"
DESCRIPTION = "Early init mounts a persistent system overlay above the read-only image, retains the base at /run/tgbs/rootfs/base, and exposes the overlay store at /var/lib/tgbs. The SysV init script prepares cgroup v2 and volatile communication channels."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://tgbs;beginline=1;endline=4;md5=391b3c3c8ef54feb728dabfc62a76739"

SRC_URI = "file://tgbs file://tgbs-preinit"

S = "${WORKDIR}"

inherit update-rc.d

INITSCRIPT_NAME = "tgbs"
INITSCRIPT_PARAMS = "start 04 S . stop 99 0 6 ."

RDEPENDS:${PN} += "busybox"

FILES:${PN} = "${sysconfdir}/init.d/tgbs ${base_sbindir}/tgbs-preinit ${localstatedir}/lib/tgbs"

do_install() {
	install -d ${D}${sysconfdir}/init.d
	install -m 0755 ${WORKDIR}/tgbs ${D}${sysconfdir}/init.d/tgbs
	install -d ${D}${base_sbindir}
	install -d -m 0700 ${D}${localstatedir}/lib/tgbs
	install -m 0755 ${WORKDIR}/tgbs-preinit ${D}${base_sbindir}/tgbs-preinit
}
