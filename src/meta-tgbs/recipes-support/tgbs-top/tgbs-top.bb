SUMMARY = "Interactive CPU monitor for TGBS domains"
DESCRIPTION = "Displays TGBS domain and task activity using libtgbs observation snapshots."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://tgbs-top.c"
S = "${WORKDIR}"
DEPENDS = "libtgbs"
CFLAGS:append = " -Wall -Wextra"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} tgbs-top.c -o tgbs-top -ltgbs
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 tgbs-top ${D}${bindir}
}
