SUMMARY = "Headless graphical image for the TGBS demonstration"
DESCRIPTION = "A minimal TGBS image with a VNC control display and an isolated Doom workload."

require recipes-core/images/core-image-minimal.bb

IMAGE_FEATURES += "read-only-rootfs"

IMAGE_INSTALL:append = " packagegroup-tgbs-runtime packagegroup-tgbs-demo"
