#!/bin/bash

export PATH="${PATH}:$(pwd)/../rumprun-10g/bin"

rm back.bin
../rumprun-10g/bin/rumprun-bake hw_netback back.bin vifconf

./create_iso.sh

xl create -c back.conf
