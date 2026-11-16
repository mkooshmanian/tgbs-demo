SUMMARY = "Command-line interface for the TGBS container runtime"
DESCRIPTION = "Create, inspect and control TGBS containers and communication channels through libtgbs."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://tgbsctl.c file://tgbsctl.h file://common.c file://run.c \
    file://attach.c file://control.c file://observe.c file://channel.c \
"

S = "${WORKDIR}"
DEPENDS = "libtgbs"
RDEPENDS:${PN} += "tgbs-runtime-init"

CFLAGS:append = " -Wall -Wextra"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} \
        tgbsctl.c common.c run.c attach.c control.c observe.c channel.c \
        -o tgbsctl -ltgbs
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 tgbsctl ${D}${bindir}
}
