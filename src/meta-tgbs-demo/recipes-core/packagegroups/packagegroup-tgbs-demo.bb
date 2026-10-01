SUMMARY = "Packages required for the TGBS demonstration"

inherit packagegroup

RDEPENDS:${PN} = " \
    dropbear \
    tgbs-demo-comm \
    tgbs-demo-doom \
    tgbs-demo-fetch \
    tgbs-demo-mixed \
    tgbs-demo-self \
"
