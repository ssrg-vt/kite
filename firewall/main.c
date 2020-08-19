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

    ifconfigd_create("bridge0");
    ifconfigd_up("bridge0");
label:
    getifaddrs(&addrs);
    tmp = addrs;

    while (tmp) {
        if (tmp->ifa_addr && tmp->ifa_addr->sa_family == AF_LINK) {
            count++;
            if (count > if_count && count != 3 && count != 2) {
                char *argv[10];
                int argc;
#if 1
                if (count != 2 && count != 3) {
                    //sprintf(ip, "10.1.1.%d/24", 10 + count);
                    sprintf(ip, "192.168.0.%d/24", 20);
                    ifconfigd_configure(tmp->ifa_name, ip);
                }
                ifconfigd_up(tmp->ifa_name);
                printf("%s is up\n", tmp->ifa_name);

                argv[0] = "brconfig";
                argv[1] = "bridge0";
                argv[2] = "add";
                argv[3] = tmp->ifa_name;
                //argv[4] = "-learn";
                //argv[5] = tmp->ifa_name;
		//argv[4] = "discover";
                //argv[5] = tmp->ifa_name;
                argv[4] = NULL;
                argc = 4;
                brconfigd(argc, argv);
                printf("%s added to bridge0\n", tmp->ifa_name);
#endif
                if_status();
                if_count = count;
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