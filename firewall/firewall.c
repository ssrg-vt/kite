#include <ifaddrs.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "brconfig/brconfig.h"
#include "ifconfig/ifconfig.h"
#include "route/route.h"
#include "arp/arp.h"
#include "sysctl/sysctl.h"

void doroute(char *ifp) {
    char *argv[12];
    int argc;

    //route add -net 192.168.0.0/24 -iface -interface 192.168.0.20
    printf("ROUTE %s\n", ifp);
    argv[0] = "route";
    argv[1] = "add";
    argv[2] = "-net";
    argv[3] = "192.168.0.15/32";
    argv[4] = "-iface";
    argv[5] = "-interface";
    argv[6] = "192.168.0.20";
    argv[7] = NULL;
    argc = 7;
    route_main(argc, argv);
}

void showroute(char *ifp) {
    char *argv[12];
    int argc;

    printf("ROUTE show");
    argv[0] = "route";
    argv[1] = "show";
    argv[2] = NULL;
    argc = 2;
    route_main(argc, argv);
}

void doarp() {
    char *argv[12];
    int argc;

    printf("ARP\n");
    argv[0] = "arp";
    argv[1] = "-a";
    argv[2] = "192.168.0.20";
    argv[3] = "00:16:3e:53:86:64";
    argv[4] = "pub";
    argv[2] = NULL;
    argc = 2;
    arp_main(argc, argv);
}

void dosysctl() {
    char *argv[12];
    int argc;

    printf("SYSCTL\n");
    //sysctl -w  net.inet.ip.forwarding=1
    argv[0] = "sysctl";
    argv[1] = "-w";
    argv[2] = "net.inet.ip.forwarding=1";
    argv[3] = NULL;
    argc = 3;
    sysctl_main(argc, argv);
}

void *ifconfigd_configure(char *iface_name, char *ip) {
    char *argv[12];
    int argc;

    printf("Configuring new interface %s\n", iface_name);

    /* Set physical interface */
    argv[0] = "ifconfigd";
    argv[1] = iface_name;
    argv[2] = ip;
    argv[3] = "netmask";
    argv[4] = "255.255.255.0";
    argv[5] = NULL;
    argc = 5;
    ifconfigd(argc, argv);

    if_status();

    return NULL;
}

void *ifconfigd_create(char *iface_name) {
    char *argv[4];
    int argc;

    printf("Creating new bridge interface %s\n", iface_name);

    /* Set physical interface */
    argv[0] = "ifconfigd";
    argv[1] = iface_name;
    argv[2] = "create";
    argv[3] = NULL;
    argc = 3;
    ifconfigd(argc, argv);

    printf("Bridge created\n");
    if_status();

    return NULL;
}

void *ifconfigd_up(char *iface_name) {
    char *argv[4];
    int argc;

    printf("Interface up %s\n", iface_name);

    /* Set physical interface */
    argv[0] = "ifconfigd";
    argv[1] = iface_name;
    argv[2] = "up";
    argv[3] = NULL;
    argc = 3;
    ifconfigd(argc, argv);

    return NULL;
}


int main(int argc, char **argv) {
    struct ifaddrs *addrs, *tmp;
    int if_count = 0, count = 0;
    char ip[24];

label:
    getifaddrs(&addrs);
    tmp = addrs;

    while (tmp) {
        if (tmp->ifa_addr && tmp->ifa_addr->sa_family == AF_LINK) {
            count++;
            if (count > if_count) {
                char *argv[10];
                int argc;
                if (count != 2) {
                    //sprintf(ip, "192.168.0.%d/24", 10 + count);
                    sprintf(ip, "192.168.0.%d/24", 20);
                    ifconfigd_configure(tmp->ifa_name, ip);
                ifconfigd_up(tmp->ifa_name);
                printf("%s is up\n", tmp->ifa_name);
                if_status();
                if_count = count;
                }
		if (count > 2){
			//doroute(tmp->ifa_name);
			//dosysctl();
			doarp();
		}
            }
        }
        tmp = tmp->ifa_next;
    }

    count = 0;
    freeifaddrs(addrs);

    for (int i = 0; i < 12; i++)
        sched_yield();
    goto label;

    exit(EXIT_SUCCESS);
}
