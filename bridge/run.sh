#!/bin/bash

export PATH="${PATH}:$(pwd)/../rumprun-netddom/bin"

#rm test
#../obj-amd64-hw-netddom/app-tools/x86_64-rumprun-netbsd-gcc -g -O0 -o test test.c

rm back.bin
#../rumprun-netddom/bin/rumprun-bake hw_netback back.bin test
../rumprun-netddom/bin/rumprun-bake hw_netback back.bin vifconf

./create_iso.sh

xl create -c back.conf
