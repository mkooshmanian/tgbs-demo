SUMMARY = "Headless graphical image for the TGBS demonstration"
DESCRIPTION = "A minimal TGBS image with a VNC control display and an isolated Doom workload."

require recipes-core/images/core-image-minimal.bb

IMAGE_FEATURES += "ssh-server-dropbear"
IMAGE_INSTALL:append = " packagegroup-tgbs-runtime"
