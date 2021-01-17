#!/bin/bash

export PATH="${PATH}:$(pwd)/../rumprun-blkback_persistent_strategy/bin"

rm vbdconf
../obj-amd64-hw-blkback_persistent_strategy/app-tools/x86_64-rumprun-netbsd-gcc -g -O0 -o vbdconf main.c

../obj-amd64-hw-blkback_persistent_strategy/app-tools/x86_64-rumprun-netbsd-cookfs -s 1 rootfs.fs rootfs

rm back.bin
rumprun-bake -m "add rootfs.fs" hw_blkback back.bin vbdconf

./create_iso.sh

xl create -c back.conf
