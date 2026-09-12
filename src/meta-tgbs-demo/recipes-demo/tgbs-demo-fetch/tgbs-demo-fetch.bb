SUMMARY = "Lightweight system summary for the TGBS demo image"
DESCRIPTION = "A dependency-free, fastfetch-style terminal summary tailored to the TGBS demonstration image."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = "file://tgbs-fetch"

S = "${WORKDIR}"

RDEPENDS:${PN} = "busybox"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${WORKDIR}/tgbs-fetch ${D}${bindir}/tgbs-fetch
}

FILES:${PN} += "${bindir}/tgbs-fetch"

