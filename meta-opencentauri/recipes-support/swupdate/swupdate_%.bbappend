FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
PACKAGE_ARCH = "${MACHINE_ARCH}"
SRC_URI += " \
    file://fragment.cfg \
    file://hwrevision \
    file://swupdate_public.pem \
    file://swupdate.cfg \
"

do_install:append() {
    install -d ${D}${sysconfdir}
    install -m 0644 ${WORKDIR}/swupdate.cfg ${D}${sysconfdir}/swupdate.cfg
    install -m 0644 ${WORKDIR}/hwrevision ${D}${sysconfdir}/hwrevision
    install -m 0644 ${WORKDIR}/swupdate_public.pem ${D}${sysconfdir}/swupdate_public.pem
}
