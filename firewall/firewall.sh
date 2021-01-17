#!/bin/bash

rm firewall

../obj-amd64-hw-pvdrivers/app-tools/x86_64-rumprun-netbsd-gcc -Wall -O0 -g -o firewall ifconfig/af_atalk.c ifconfig/af_inet6.c ifconfig/ieee80211.c ifconfig/ifconfig.c ifconfig/env.c ifconfig/parse.c ifconfig/carp.c ifconfig/agr.c ifconfig/ether.c ifconfig/af_inetany.c ifconfig/media.c ifconfig/tunnel.c ifconfig/ifconfig_hostops.c ifconfig/af_inet.c ifconfig/vlan.c ifconfig/af_link.c ifconfig/util.c brconfig/brconfig.c  route/keywords.c  route/route.c  route/route_hostops.c  route/rtutil.c  route/show.c sysctl/sysctl.c  sysctl/sysctl_hostops.c  arp/arp.c arp/arp_hostops.c firewall.c -DPORTMAP -DRUMPRUN -lprop -lutil
