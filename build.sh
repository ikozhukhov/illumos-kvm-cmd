#!/bin/bash
#
# Copyright (c) 2011, Joyent Inc., All rights reserved.
#

for dir in seabios vgabios kvm/test; do
    cp roms/${dir}/config.mak.tmpl roms/${dir}/config.mak
done

PNGDIR="${PWD}/libpng-1.5.4"
PNGINC="${PNGDIR}/proto/usr/local/include"
PNGLIB="${PNGDIR}/proto/usr/local/lib"

if [[ ! -d ${PNGDIR} ]]; then
    (curl ftp://ftp.simplesystems.org/pub/libpng/png/src/libpng-1.5.4.tar.gz | \
        tar -zxf -)
    if [[ $? != "0" || ! -d ${PNGDIR} ]]; then
        echo "Failed to get libpng."
        rm -rf ${PNGDIR}
        exit 1
    fi
fi

if [[ ! -e ${PNGLIB}/libpng.a ]]; then
    (cd ${PNGDIR} && \
        LDFLAGS=-m64 CFLAGS=-m64 ./configure --disable-shared && \
        make && \
        mkdir -p ${PNGDIR}/proto && \
        make DESTDIR=${PNGDIR}/proto install)
fi

echo "==> Running configure"
./configure \
    --extra-cflags="-I${PNGDIR}/proto/usr/local/include" \
    --extra-ldflags="-L${PNGDIR}/proto/usr/local/lib -lz -lm" \
    --prefix=/smartdc \
    --audio-card-list= \
    --audio-drv-list= \
    --disable-bluez \
    --disable-brlapi \
    --disable-curl \
    --enable-debug \
    --enable-kvm \
    --enable-kvm-pit \
    --enable-vnc-png \
    --disable-kvm-device-assignment \
    --disable-sdl \
    --disable-vnc-jpeg \
    --disable-vnc-sasl \
    --disable-vnc-tls \
    --enable-trace-backend=dtrace \
    --kerneldir=$(cd `pwd`/../kvm; pwd) \
    --cpu=x86_64

if [[ $? != 0 ]]; then
	echo "Failed to configure, bailing"
	exit 1
fi

echo "==> Make"
gmake
