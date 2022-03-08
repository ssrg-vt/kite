#!/bin/bash

export PATH="${PATH}:$(pwd)/../rumprun/bin"

rm vbdconf
../obj-amd64-hw/app-tools/x86_64-rumprun-netbsd-gcc -g -O0 -o vbdconf main.c

rm back.bin
rumprun-bake hw_blkback back.bin vbdconf

./create_iso.sh

xl create -c back.conf
