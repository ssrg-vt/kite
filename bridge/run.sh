#!/bin/bash

export PATH="${PATH}:$(pwd)/../rumprun/bin"

rm back.bin
../rumprun/bin/rumprun-bake hw_netback back.bin vifconf

./create_iso.sh

xl create -c back.conf
