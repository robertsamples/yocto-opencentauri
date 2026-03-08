FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += " \
    file://fstab \
    file://motd-opencentauri \
"

do_install:append() {
    install -d -m 0755 ${D}/mnt/writable
    install -d -m 0755 ${D}/printer_data
    install -m 0644 ${WORKDIR}/motd-opencentauri ${D}${sysconfdir}/motd
}
