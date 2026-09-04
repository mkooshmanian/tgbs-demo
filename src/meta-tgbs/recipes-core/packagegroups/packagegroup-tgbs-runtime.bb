SUMMARY = "Runtime packages for TGBS"
DESCRIPTION = "Installs TGBS control and runtime init."

LICENSE = "MIT"

inherit packagegroup

RDEPENDS:${PN} = " \
    tgbs-runtime-init \
    tgbsctl \
"
