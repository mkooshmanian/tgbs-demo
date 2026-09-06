SUMMARY = "TGBS Doom demonstration"
DESCRIPTION = "Chocolate Doom, Xvfb and x11vnc running inside a TGBS temporal domain."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://tgbs-demo-doom \
    file://tgbs-demo-doom-entrypoint \
"

S = "${WORKDIR}"

RDEPENDS:${PN} = " \
    xserver-xorg-xvfb \
    xkbcomp \
    x11vnc \
    chocolate-doom \
    doom-iwad \
"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 \
        ${WORKDIR}/tgbs-demo-doom \
        ${D}${bindir}/tgbs-demo-doom

    install -d ${D}${libexecdir}/tgbs-demo
    install -m 0755 \
        ${WORKDIR}/tgbs-demo-doom-entrypoint \
        ${D}${libexecdir}/tgbs-demo/doom-entrypoint
}

FILES:${PN} += " \
    ${libexecdir}/tgbs-demo/doom-entrypoint \
"