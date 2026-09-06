SUMMARY = "Chocolate Doom is a port of the Doom game engine from id Software."
DESCRIPTION = "Chocolate Doom aim is to accurately reproduce the experience of \
playing the original DOS Doom from id Software. It is a conservative, \
historically accurate Doom source port, which is compatible with the thousands \
of mods and levels that were made before the Doom source code was released."
HOMEPAGE = "https://www.chocolate-doom.org/"
BUGTRACKER = "https://github.com/chocolate-doom/chocolate-doom/issues"

SECTION = "games"

LICENSE = "GPL-2.0-or-later"
LIC_FILES_CHKSUM = "file://COPYING;md5=b234ee4d69f5fce4486a80fdaf4a4263"

DEPENDS = "virtual/libsdl2 libsdl2-mixer libsdl2-net"
REQUIRED_DISTRO_FEATURES = "x11"

SRCBRANCH = "chocolate-doom-3.0"
SRC_URI = " \
    git://github.com/chocolate-doom/chocolate-doom.git;protocol=https;branch=${SRCBRANCH} \
    file://bump-version-update-news-3.0.1.patch \
    file://remove-redundant-demoextend-definition.patch \
"
SRCREV = "166fa495daa5392ffe55a3e9c204953234aff45b"
S = "${WORKDIR}/git"

inherit autotools-brokensep features_check gettext pkgconfig

FILES:${PN} += " \
    ${datadir}/appdata \
    ${datadir}/bash-completion \
    ${datadir}/icons \
"
