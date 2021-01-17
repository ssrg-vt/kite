#include <stdio.h>
#include <sched.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>

int main(void) {
	char ch[100];
	struct ifaddrs *addrs,*tmp;

label:	printf("Interface list: ");
	getifaddrs(&addrs);
	tmp = addrs;

	while (tmp) {
		if (tmp->ifa_addr && tmp->ifa_addr->sa_family == AF_LINK)
			printf("%s, ", tmp->ifa_name);

		        tmp = tmp->ifa_next;
	}

	printf("\b\b \n");
	freeifaddrs(addrs);

	for(int i=0; i<12; i++)
		sched_yield();

//	sleep(2);
	goto label;

	return 0;
}
