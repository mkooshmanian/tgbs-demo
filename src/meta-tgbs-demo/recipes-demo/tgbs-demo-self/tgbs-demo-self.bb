SUMMARY = "Interactive TGBS container demonstration"
DESCRIPTION = "Opens an interactive shell as PID 1 in a namespaced TGBS temporal domain."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://tgbs-demo-self \
    file://tgbs-demo-self-entrypoint \
"

S = "${WORKDIR}"

RDEPENDS:${PN} = "packagegroup-tgbs-runtime"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 \
        ${WORKDIR}/tgbs-demo-self \
        ${D}${bindir}/tgbs-demo-self

    install -d ${D}${libexecdir}/tgbs-demo
    install -m 0755 \
        ${WORKDIR}/tgbs-demo-self-entrypoint \
        ${D}${libexecdir}/tgbs-demo/self-entrypoint
}

FILES:${PN} += "${libexecdir}/tgbs-demo/self-entrypoint"
