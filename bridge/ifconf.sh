#!/bin/bash

rm vifconf

../obj-amd64-hw-netddom/app-tools/x86_64-rumprun-netbsd-gcc -Wall -O0 -g -o vifconf ifconfig/af_atalk.c ifconfig/af_inet6.c ifconfig/ieee80211.c ifconfig/ifconfig.c ifconfig/env.c ifconfig/parse.c ifconfig/carp.c ifconfig/agr.c ifconfig/ether.c ifconfig/af_inetany.c ifconfig/media.c ifconfig/tunnel.c ifconfig/ifconfig_hostops.c ifconfig/af_inet.c ifconfig/vlan.c ifconfig/af_link.c ifconfig/util.c brconfig/brconfig.c main.c -DPORTMAP -DRUMPRUN -lprop -lutil
