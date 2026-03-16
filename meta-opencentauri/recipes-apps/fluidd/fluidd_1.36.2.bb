SUMMARY = "Fluidd - Web Interface for Klipper"
DESCRIPTION = "Fluidd is a free and open-source Klipper web interface for \
    managing your 3D printer. Features responsive UI supporting desktop, \
    tablets and mobile with customizable layouts."
HOMEPAGE = "https://github.com/fluidd-core/fluidd"
LICENSE = "GPL-3.0-only"
LIC_FILES_CHKSUM = "file://index.html;md5=641200d3fbd91b44d56ad8c7a74f25e5"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI = "https://github.com/fluidd-core/fluidd/releases/download/v${PV}/fluidd.zip;subdir=fluidd \
    file://fluidd-nginx \
    file://fluidd.cfg \
"
SRC_URI[sha256sum] = "3f9dbe9ac5db51d1025a5737b207d83558c07cb0d7af34d3c5832c868fa72454"

S = "${WORKDIR}/fluidd"

RDEPENDS:${PN} = " \
    nginx \
    moonraker \
"

do_configure() {
    :
}

do_compile() {
    :
}

do_install() {
    # Install static web files
    install -d ${D}/var/www/fluidd
    cp -r ${S}/* ${D}/var/www/fluidd/

    # Install nginx site config
    install -d ${D}${sysconfdir}/nginx/sites-available
    install -d ${D}${sysconfdir}/nginx/sites-enabled

    cp ${WORKDIR}/fluidd-nginx ${D}${sysconfdir}/nginx/sites-available/fluidd

    # Symlink to sites-enabled
    ln -sf ${sysconfdir}/nginx/sites-available/fluidd \
        ${D}${sysconfdir}/nginx/sites-enabled/fluidd

    # Install default fluidd config
    install -d ${D}${sysconfdir}/fluidd
    cp ${WORKDIR}/fluidd.cfg ${D}${sysconfdir}/fluidd
}

FILES:${PN} = " \
    /var/www/fluidd \
    ${sysconfdir}/nginx/sites-available/fluidd \
    ${sysconfdir}/nginx/sites-enabled/fluidd \
    ${sysconfdir}/fluidd/fluidd.cfg \
"

CONFFILES:${PN} = " \
    ${sysconfdir}/nginx/sites-available/fluidd \
    ${sysconfdir}/fluidd/fluidd.cfg \
"
