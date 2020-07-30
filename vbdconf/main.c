#include <stdio.h>
#include <sched.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mount.h>

#include "../platform/hw/librumpxen_blkback/xbdback_virt.h"

physical_device rumpns_storage_devices[10];

int main(void) 
{
    struct dirent *de;
    DIR *dir;
    int i = 0;
    struct stat sb;

    printf("This is the test program \n");
    
    dir = opendir("/dev");            
    if (dir == NULL)  // opendir returns NULL if couldn't open directory 
    { 
    	printf("Could not open current directory" ); 
    	return 0; 
    }

    while ((de = readdir(dir)) != NULL)
    {
	if(strstr(de->d_name, "ld")) 
	{
		snprintf(rumpns_storage_devices[i].path, 
				sizeof(rumpns_storage_devices[i].path),
				"/dev/%s", de->d_name);
	 
		stat(rumpns_storage_devices[i].path, &sb);
		rumpns_storage_devices[i].number = sb.st_rdev;

		printf("%d %s %lu\n", i, rumpns_storage_devices[i].path,
				rumpns_storage_devices[i].number); 

		rumpns_storage_devices[i].status = 1;
		i++;
    	}
    }

label:
	for(int i=0; i<12000; i++)
		sched_yield();
	goto label;

	return 0;
}
