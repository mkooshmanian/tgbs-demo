SUMMARY = "TGBS queuing and sampling communication demonstration"
DESCRIPTION = "Two TGBS domains exchange messages over queuing channels and subscribe to a sampling channel published by a third domain."

LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://tgbs-demo-comm \
    file://tgbs-demo-comm-node.c \
"

S = "${WORKDIR}"

DEPENDS = "libtgbscomm"
RDEPENDS:${PN} = "packagegroup-tgbs-runtime"

CFLAGS += "-Wall -Wextra -pthread"

do_compile() {
    ${CC} ${CFLAGS} -o ${B}/tgbs-demo-comm-node \
        ${WORKDIR}/tgbs-demo-comm-node.c ${LDFLAGS} -pthread -ltgbscomm
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 \
        ${WORKDIR}/tgbs-demo-comm \
        ${D}${bindir}/tgbs-demo-comm

    install -d ${D}${libexecdir}/tgbs-demo
    install -m 0755 \
        ${B}/tgbs-demo-comm-node \
        ${D}${libexecdir}/tgbs-demo/comm-node
}

FILES:${PN} += "${libexecdir}/tgbs-demo/comm-node"
