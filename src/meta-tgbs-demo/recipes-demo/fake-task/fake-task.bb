SUMMARY = "Fake task to simulate CPU workloads"
DESCRIPTION = "Small program that simulates a periodic or continuous task. \
It spends a fixed or randomized CPU budget per job (spin work step) and emits \
start/finish/done records over a Unix datagram socket for tracing."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://fake-task.c \
"

S = "${WORKDIR}"

CFLAGS += "-Wall"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -o ${B}/fake-task ${WORKDIR}/fake-task.c -lm
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/fake-task ${D}${bindir}/fake-task
}

FILES:${PN} += "${bindir}/fake-task"
