SUMMARY = "TGBS mixed workload demonstration"
DESCRIPTION = "Static mixed-policy workload composed of periodic RT tasks and continuous FAIR background tasks, executed inside a TGBS domain."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://tgbs-demo-mixed \
    file://tgbs-demo-mixed-entrypoint \
"

S = "${WORKDIR}"

RDEPENDS:${PN} = " \
    packagegroup-tgbs-runtime \
    fake-task \
"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 \
        ${WORKDIR}/tgbs-demo-mixed \
        ${D}${bindir}/tgbs-demo-mixed

    install -d ${D}${libexecdir}/tgbs-demo
    install -m 0755 \
        ${WORKDIR}/tgbs-demo-mixed-entrypoint \
        ${D}${libexecdir}/tgbs-demo/mixed-entrypoint
}

FILES:${PN} += " \
    ${libexecdir}/tgbs-demo/mixed-entrypoint \
"