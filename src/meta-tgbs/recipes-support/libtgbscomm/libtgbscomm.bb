SUMMARY = "Communication channel library for TGBS domains"
DESCRIPTION = "libtgbscomm provides an immutable-contract communication API backed by unidirectional AF_UNIX datagram channels."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://tgbscomm.c file://channel.h file://tgbscomm.pc"

S = "${WORKDIR}"

CFLAGS:append = " -Wall -Wextra"
# Keep the packagegroup dependency stable instead of renaming it from the SONAME.
DEBIAN_NOAUTONAME:${PN} = "1"

do_compile() {
	${CC} ${CFLAGS} -fPIC -shared ${LDFLAGS} \
		-Wl,-soname,libtgbscomm.so.0 \
		-o libtgbscomm.so.0.1.0 tgbscomm.c
	ln -sf libtgbscomm.so.0.1.0 libtgbscomm.so.0
	ln -sf libtgbscomm.so.0 libtgbscomm.so
}

do_install() {
	install -d ${D}${libdir} ${D}${includedir}/tgbs ${D}${libdir}/pkgconfig
	install -m 0755 libtgbscomm.so.0.1.0 ${D}${libdir}
	ln -sf libtgbscomm.so.0.1.0 ${D}${libdir}/libtgbscomm.so.0
	ln -sf libtgbscomm.so.0 ${D}${libdir}/libtgbscomm.so
	install -m 0644 channel.h ${D}${includedir}/tgbs/channel.h
	install -m 0644 tgbscomm.pc ${D}${libdir}/pkgconfig
}

FILES:${PN} = "${libdir}/libtgbscomm.so.0*"
FILES:${PN}-dev += " \
	${includedir}/tgbs/channel.h \
	${libdir}/libtgbscomm.so \
	${libdir}/pkgconfig/tgbscomm.pc \
"
