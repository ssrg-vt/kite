#!/bin/bash

export PATH="${PATH}:$(pwd)/../rumprun-netddom/bin"

rm back.bin
../rumprun-netddom/bin/rumprun-bake hw_netback back.bin vifconf

./create_iso.sh

xl create -c back.conf
