FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += "file://dropbear-default"

do_install:append() {
    install -d ${D}${sysconfdir}/default
    install -m 0644 ${WORKDIR}/dropbear-default ${D}${sysconfdir}/default/dropbear
}
