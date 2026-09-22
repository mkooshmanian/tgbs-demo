SUMMARY = "Interactive CPU monitor for TGBS domains"
DESCRIPTION = "A small top-like monitor focused on tgbsctl-managed cgroups, their CPU reservations, and the tasks running inside them."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://tgbs-top.c"

S = "${WORKDIR}"

RDEPENDS:${PN} += "tgbsctl"

CFLAGS += "-Wall -Wextra"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -o ${B}/tgbs-top ${WORKDIR}/tgbs-top.c
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/tgbs-top ${D}${bindir}/tgbs-top
}

FILES:${PN} += "${bindir}/tgbs-top"
