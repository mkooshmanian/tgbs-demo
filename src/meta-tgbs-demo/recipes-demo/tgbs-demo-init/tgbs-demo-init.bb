SUMMARY = "Automatic startup of the TGBS demonstrations"
DESCRIPTION = "SysV init service launching Doom, mixed and communication domains from a tgbs-launcher configuration."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://tgbs-demo file://launcher.json"
S = "${WORKDIR}"

inherit update-rc.d

INITSCRIPT_NAME = "tgbs-demo"
INITSCRIPT_PARAMS = "start 99 2 3 4 5 . stop 01 0 1 6 ."

RDEPENDS:${PN} = " \
    tgbs-launcher \
    tgbsctl \
    tgbs-demo-doom \
    tgbs-demo-mixed \
    tgbs-demo-comm \
    jq \
"

do_install() {
    install -d ${D}${sysconfdir}/init.d ${D}${sysconfdir}/tgbs-demo
    install -m 0755 ${WORKDIR}/tgbs-demo ${D}${sysconfdir}/init.d/tgbs-demo
    install -m 0644 ${WORKDIR}/launcher.json ${D}${sysconfdir}/tgbs-demo/launcher.json
}

CONFFILES:${PN} = "${sysconfdir}/tgbs-demo/launcher.json"
