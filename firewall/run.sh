#!/bin/bash

export PATH="${PATH}:$(pwd)/../rumprun-netfront/bin"

rm back.bin
#../rumprun-netfront/bin/rumprun-bake hw_netback back.bin vifconf
#../rumprun-netfront/bin/rumprun-bake hw_netdaisy back.bin vifconf
../rumprun-netfront/bin/rumprun-bake hw_netdaisy back.bin firewall 

./create_iso.sh

xl create -c back.conf
