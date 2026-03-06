require kalico_${PV}.inc

SUMMARY = "Kalico 3D Printer Firmware"
DESCRIPTION = "Klipper, but Limitless"

SRC_URI += " file://config.bed"

DEPENDS += " gcc-arm-none-eabi-native"

RPROVIDES:${PN} += "klipper-firmware-bed"

EXTRA_OEMAKE += " KCONFIG_CONFIG=../config.bed"

do_install() {
    install -d ${D}/lib/firmware
    cp -r ${S}/out/klipper.elf ${D}/lib/firmware/klipper-bed.elf
}

FILES:${PN} = "/lib/firmware/klipper-bed.elf"
