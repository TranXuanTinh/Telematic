SUMMARY = "Automotive TCU Yocto Linux Production & QEMU Reference Image"
DESCRIPTION = "Minimal hardened systemd + RAUC Dual-Bank A/B Linux OS image with the 6 TCU Telematics Daemons."
LICENSE = "MIT"

inherit core-image

IMAGE_FEATURES += "read-only-rootfs ssh-server-openssh"

WKS_FILE = "tcu-dualbank-ab.wks"
IMAGE_FSTYPES = "ext4 wic.gz wic.bmap"

IMAGE_INSTALL:append = " \
    packagegroup-core-boot \
    tcu-suite \
    rauc \
    can-utils \
    iproute2 \
    sqlite3 \
    openssl-bin \
    ca-certificates \
"

