#!/bin/bash

export PATH="${PATH}:$(pwd)/../rumprun-pvdrivers/bin"

rm back.bin
../rumprun-pvdrivers/bin/rumprun-bake hw_netback back.bin vifconf

./create_iso.sh

#xl create -c back.conf
