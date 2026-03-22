HOMEPAGE = "https://kalico.gg/"
LICENSE = "GPL-3.0-only"
LIC_FILES_CHKSUM = "file://COPYING;md5=1ebbd3e34237af26da5dc08a4e440464"

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI = "git://github.com/OpenCentauri/kalico.git;protocol=https;branch=rpmsg-with-new-hx71x"
SRCREV = "f66de876fe17b84cfc1401dafebf1260b28e9e6f"

PR = "r3"

S = "${WORKDIR}/git"

DEPENDS = " \
    python3-native \
"
