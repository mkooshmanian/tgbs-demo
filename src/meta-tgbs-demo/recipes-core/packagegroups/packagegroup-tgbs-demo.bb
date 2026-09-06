SUMMARY = "Packages required for the TGBS demonstration"

inherit packagegroup

RDEPENDS:${PN} = " \
    dropbear \
    tgbs-demo-doom \
    tgbs-demo-mixed \
"