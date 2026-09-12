SUMMARY = "Interactive CPU monitor for TGBS domains"
DESCRIPTION = "A small top-like monitor focused on tgbsctl-managed cgroups, their CPU reservations, and the tasks running inside them."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://tgbs-demo-top.c"

S = "${WORKDIR}"

CFLAGS += "-Wall -Wextra"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -o ${B}/tgbs-demo-top ${WORKDIR}/tgbs-demo-top.c
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/tgbs-demo-top ${D}${bindir}/tgbs-demo-top
}

FILES:${PN} += "${bindir}/tgbs-demo-top"
