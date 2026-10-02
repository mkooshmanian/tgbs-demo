SUMMARY = "Communication channel library for TGBS domains"
DESCRIPTION = "libtgbscomm provides separate queuing and sampling channel APIs, shared immutable-contract validation, an AF_UNIX datagram queuing backend, and sampling API scaffolding."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

FILESEXTRAPATHS:prepend := "${THISDIR}/../channel-common/files:"

SRC_URI = "file://queuing.c file://queuing.h file://sampling.c file://sampling.h file://types.h file://comm-internal.c file://comm-internal.h file://channel-contract.c file://channel-contract.h file://tgbscomm.pc"

S = "${WORKDIR}"

CFLAGS:append = " -Wall -Wextra"
# Keep the packagegroup dependency stable instead of renaming it from the SONAME.
DEBIAN_NOAUTONAME:${PN} = "1"

do_compile() {
	${CC} ${CFLAGS} -fPIC -shared ${LDFLAGS} \
		-Wl,-soname,libtgbscomm.so.0 \
		-o libtgbscomm.so.0.2.0 queuing.c sampling.c comm-internal.c channel-contract.c
	ln -sf libtgbscomm.so.0.2.0 libtgbscomm.so.0
	ln -sf libtgbscomm.so.0 libtgbscomm.so
}

do_install() {
	install -d ${D}${libdir} ${D}${includedir}/tgbs ${D}${libdir}/pkgconfig
	install -m 0755 libtgbscomm.so.0.2.0 ${D}${libdir}
	ln -sf libtgbscomm.so.0.2.0 ${D}${libdir}/libtgbscomm.so.0
	ln -sf libtgbscomm.so.0 ${D}${libdir}/libtgbscomm.so
	install -m 0644 types.h queuing.h sampling.h ${D}${includedir}/tgbs/
	install -m 0644 tgbscomm.pc ${D}${libdir}/pkgconfig
}

FILES:${PN} = "${libdir}/libtgbscomm.so.0*"
FILES:${PN}-dev += " \
	${includedir}/tgbs/ \
	${libdir}/libtgbscomm.so \
	${libdir}/pkgconfig/tgbscomm.pc \
"
