require kalico_${PV}.inc

SUMMARY = "Kalico 3D Printer Firmware"
DESCRIPTION = "Klipper, but Limitless"

SRC_URI += " file://config.toolhead"


DEPENDS += " gcc-arm-none-eabi-native"

RPROVIDES:${PN} += "klipper-firmware-toolhead"

# INHIBIT_DEFAULT_DEPS = "1"
# INHIBIT_SYSROOT_STRIP = "1"
# INHIBIT_PACKAGE_DEBUG_SPLIT = "1"

# COMPATIBLE_MACHINE = ".*"

# TOOLCHAIN = ""
# FW_TARGET_PREFIX = "arm-none-eabi-"

# PREFERRED_PROVIDER_virtual/arm-none-eabi-binutils = ""
# PREFERRED_PROVIDER_virtual/arm-none-eabi-gcc = ""

# TOOLCHAIN_PATH = "${RECIPE_SYSROOT_NATIVE}/usr/bin/arm-gnu-toolchain-13.2.Rel1-x86_64-arm-none-eabi/bin"

EXTRA_OEMAKE += " KCONFIG_CONFIG=../config.toolhead"
# EXTRA_OEMAKE += " CROSS_PREFIX=${TOOLCHAIN_PATH}/${TARGET_PREFIX}"

do_install() {
    install -d ${D}/lib/firmware
    cp -r ${S}/out/klipper.elf ${D}/lib/firmware/klipper-toolhead.elf
}

FILES:${PN} = "/lib/firmware/klipper-toolhead.elf"
