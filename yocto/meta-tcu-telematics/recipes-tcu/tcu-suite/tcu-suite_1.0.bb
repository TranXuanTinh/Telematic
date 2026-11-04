SUMMARY = "Automotive Telematics Control Unit (TCU) Multi-Daemon C++17 Suite"
DESCRIPTION = "Implements CEN EN 15722 eCall, UN ECE R156 Dual-Domain OTA, Stolen Vehicle Tracking with Safe Zero-Speed Immobilizer, and ISO 14229 UDS Test & BIST Services."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

PROVIDES += "tcu-telematics"
RPROVIDES:${PN} += "tcu-telematics"

DEPENDS = "openssl sqlite3 yaml-cpp spdlog fmt curl"
RDEPENDS:${PN} = "rauc sqlite3 openssl-bin can-utils"

inherit cmake pkgconfig systemd useradd

# Reference local workspace tree via externalsrc or tarball/git in CI
TCU_LOCAL_SRC ?= "${@os.path.abspath(os.path.join('${THISDIR}', '../../../..'))}"
inherit externalsrc
EXTERNALSRC = "${TCU_LOCAL_SRC}"

# On qemuarm64 default to Mock/Virtual MCU mode; on SA515M NAD BSP switch USE_MOCK_HAL=OFF
TCU_MOCK_DEFAULT = "${@bb.utils.contains('MACHINE', 'qemuarm64', 'ON', 'OFF', d)}"

EXTRA_OECMAKE = " \
    -DUSE_MOCK_HAL=${TCU_MOCK_DEFAULT} \
    -DENABLE_SA515M_HAL=ON \
    -DBUILD_TESTS=OFF \
"

USERADD_PACKAGES = "${PN}"
GROUPADD_PARAM:${PN} = "-r tcu; -r spi"
USERADD_PARAM:${PN} = "--system --no-create-home --home-dir /run/tcu --shell /sbin/nologin -g tcu -G dialout,audio,spi tcu"

SYSTEMD_PACKAGES = "${PN}"
SYSTEMD_SERVICE:${PN} = " \
    tcu-telematics.target \
    tcu-mcu-ipcd.service \
    tcu-platformd.service \
    tcu-ecalld.service \
    tcu-otad.service \
    tcu-svtd.service \
    tcu-diagd.service \
"
SYSTEMD_AUTO_ENABLE:${PN} = "enable"

do_install:append() {
    install -d ${D}/data/db
    install -d ${D}/data/ota_staging
    install -d ${D}/factory
    chown -R tcu:tcu ${D}/data
}

FILES:${PN} += " \
    ${bindir}/tcu-* \
    ${sysconfdir}/tcu/*.yaml \
    ${systemd_system_unitdir}/tcu-*.service \
    ${systemd_system_unitdir}/tcu-telematics.target \
    /data \
    /factory \
"

