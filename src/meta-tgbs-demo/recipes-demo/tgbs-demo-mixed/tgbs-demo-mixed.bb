SUMMARY = "Configurable TGBS mixed workload demonstration"
DESCRIPTION = "JSON-configured mixed-policy workload composed of periodic RT tasks and continuous FAIR background tasks, executed inside a TGBS domain."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://tgbs-demo-mixed \
    file://tgbs-demo-mixed-entrypoint \
    file://tgbs-demo-mixed-timeline.c \
    file://default.json \
"

S = "${WORKDIR}"

CFLAGS += "-Wall -Wextra"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -o ${B}/tgbs-demo-mixed-timeline \
        ${WORKDIR}/tgbs-demo-mixed-timeline.c
}

RDEPENDS:${PN} = " \
    packagegroup-tgbs-runtime \
    fake-task \
    jq \
"

do_install() {
    install -d ${D}${bindir}
    install -m 0755 \
        ${WORKDIR}/tgbs-demo-mixed \
        ${D}${bindir}/tgbs-demo-mixed
    install -m 0755 \
        ${B}/tgbs-demo-mixed-timeline \
        ${D}${bindir}/tgbs-demo-mixed-timeline

    install -d ${D}${libexecdir}/tgbs-demo
    install -m 0755 \
        ${WORKDIR}/tgbs-demo-mixed-entrypoint \
        ${D}${libexecdir}/tgbs-demo/mixed-entrypoint

    install -d ${D}${sysconfdir}/tgbs-demo/mixed
    install -m 0644 \
        ${WORKDIR}/default.json \
        ${D}${sysconfdir}/tgbs-demo/mixed/default.json
}

FILES:${PN} += " \
    ${libexecdir}/tgbs-demo/mixed-entrypoint \
    ${sysconfdir}/tgbs-demo/mixed/default.json \
"

CONFFILES:${PN} += "${sysconfdir}/tgbs-demo/mixed/default.json"
