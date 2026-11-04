SUMMARY = "Signed RAUC A/B RootFS OTA Update Bundle for Automotive TCU"
DESCRIPTION = "Packages tcu-telematics-image into a cryptographically signed .raucb bundle compliant with UN ECE R156."
LICENSE = "MIT"

inherit bundle

RAUC_BUNDLE_COMPATIBLE = "sa515m-s32k144-revB"
RAUC_BUNDLE_VERSION = "2.4.0"
RAUC_BUNDLE_DESCRIPTION = "Automotive TCU Dual-Bank RootFS OTA Update Bundle"

RAUC_BUNDLE_SLOTS = "rootfs"
RAUC_SLOT_rootfs = "tcu-telematics-image"
RAUC_SLOT_rootfs[fstype] = "ext4"

# Override in conf/local.conf with OEM PKI / HSM signing key paths
RAUC_KEY_FILE ?= "${THISDIR}/files/dev-ota-key.pem"
RAUC_CERT_FILE ?= "${THISDIR}/files/dev-ota-cert.pem"

