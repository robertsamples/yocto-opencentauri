HOMEPAGE = "https://kalico.gg/"
LICENSE = "GPL-3.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=1ebbd3e34237af26da5dc08a4e440464"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI = "git://github.com/OpenCentauri/kalico.git;protocol=https;branch=open-centauri"
SRCREV = "415633afd9c1fac96bbb26c43865e8e600b6b8ae"

S = "${WORKDIR}/git"

DEPENDS = " \
    python3-native \
"
