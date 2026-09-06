SUMMARY = "Doom Shareware IWAD"
DESCRIPTION = "Installs the Doom 1.9 shareware IWAD used by the KosmoDoom demo"
LICENSE = "CLOSED"

PV = "1.9"

SRC_URI = "http://www.jbserver.com/downloads/games/doom/misc/shareware/doom19s.zip;downloadfilename=doom19s.zip"
SRC_URI[sha256sum] = "cacf0142b31ca1af00796b4a0339e07992ac5f21bc3f81e7532fe1b5e1b486e6"

S = "${WORKDIR}"

inherit allarch

DEPENDS += "unzip-native"

do_extract_wad() {
    rm -f ${WORKDIR}/doom-${PV}.zip
    rm -f ${WORKDIR}/DOOM1.WAD

    unzip -p ${DL_DIR}/doom19s.zip 'DOOMS_19.[12]' \
        > ${WORKDIR}/doom-${PV}.zip

    unzip -o ${WORKDIR}/doom-${PV}.zip \
        -d ${WORKDIR} DOOM1.WAD
}

addtask extract_wad after do_unpack before do_install

do_extract_wad[depends] += "unzip-native:do_populate_sysroot"

do_install() {
    install -d ${D}${datadir}/games/doom

    install -m 0644 ${WORKDIR}/DOOM1.WAD \
        ${D}${datadir}/games/doom/doom1.wad
}

FILES:${PN} = "${datadir}/games/doom/doom1.wad"