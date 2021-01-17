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
//#include "arp/arp.h"
//#include "sysctl/sysctl.h"

void doroute() {
    char *argv[12];
    int argc;

    printf("ROUTE\n");
    argv[0] = "route";
    argv[1] = "show";
    argv[2] = "-v";
    argv[3] = NULL;
    argc = 3;
    route_main(argc, argv);
}

void doarp() {
    char *argv[12];
    int argc;

    printf("ARP\n");
    argv[0] = "arp";
    argv[1] = "-a";
    argv[2] = NULL;
    argc = 2;
    //arp_main(argc, argv);
}

void dosysctl() {
    char *argv[12];
    int argc;

    printf("SYSCTL\n");
    argv[0] = "sysctl";
    argv[1] = NULL;
    argc = 1;
   // sysctl_main(argc, argv);
}

int main(int argc, char **argv) {
    printf("Firewall VM\n");
    
    doroute();
    //doarp();
    dosysctl();

    exit(EXIT_SUCCESS);
}
