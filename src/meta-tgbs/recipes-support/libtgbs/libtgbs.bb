SUMMARY = "Daemonless TGBS container and channel management library"
DESCRIPTION = "Native C API for container lifecycle, TGBS resource control, observation and immutable channel administration."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://domain.h file://channel.h file://internal.h \
    file://cgroup.c file://control.c file://observe.c \
    file://run.c file://exec.c file://rootfs.c file://bind.c file://channel.c \
    file://cpu-runtime.c file://cpu-runtime.h \
    file://channel-contract.c file://channel-contract.h \
    file://queuing-format.h file://sampling-format.h \
    file://exports.map file://tgbs.pc \
"
S = "${WORKDIR}"

CFLAGS:append = " -Wall -Wextra"
DEBIAN_NOAUTONAME:${PN} = "1"
RDEPENDS:${PN} += "tgbs-runtime-init"

do_compile() {
    ${CC} ${CFLAGS} -fPIC -shared ${LDFLAGS} \
        -Wl,-soname,libtgbs.so.0 -Wl,--version-script=exports.map \
        -o libtgbs.so.0.1.0 \
        cgroup.c control.c observe.c run.c exec.c rootfs.c bind.c \
        channel.c cpu-runtime.c channel-contract.c
}

do_install() {
    install -d ${D}${libdir} ${D}${includedir}/tgbs ${D}${libdir}/pkgconfig
    install -m 0755 libtgbs.so.0.1.0 ${D}${libdir}
    ln -sf libtgbs.so.0.1.0 ${D}${libdir}/libtgbs.so.0
    ln -sf libtgbs.so.0 ${D}${libdir}/libtgbs.so
    install -m 0644 domain.h channel.h ${D}${includedir}/tgbs/
    sed -e 's|^prefix=.*|prefix=${prefix}|' \
        -e 's|^libdir=.*|libdir=${libdir}|' \
        -e 's|^includedir=.*|includedir=${includedir}|' \
        tgbs.pc > ${D}${libdir}/pkgconfig/tgbs.pc
}

FILES:${PN} = "${libdir}/libtgbs.so.0*"
FILES:${PN}-dev += " \
    ${includedir}/tgbs/domain.h \
    ${includedir}/tgbs/channel.h \
    ${libdir}/libtgbs.so \
    ${libdir}/pkgconfig/tgbs.pc \
"
