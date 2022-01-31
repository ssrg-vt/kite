#!/bin/bash

rm vifconf

../obj-amd64-hw/app-tools/x86_64-rumprun-netbsd-gcc -Wall -O0 -g -o vifconf ifconfig_src/af_atalk.c ifconfig_src/af_inet6.c ifconfig_src/ieee80211.c ifconfig_src/ifconfig.c ifconfig_src/env.c ifconfig_src/parse.c ifconfig_src/carp.c ifconfig_src/agr.c ifconfig_src/ether.c ifconfig_src/af_inetany.c ifconfig_src/media.c ifconfig_src/tunnel.c ifconfig_src/ifconfig_hostops.c ifconfig_src/af_inet.c ifconfig_src/vlan.c ifconfig_src/af_link.c ifconfig_src/util.c brconfig_src/brconfig.c main.c -DPORTMAP -DRUMPRUN -lprop -lutil
