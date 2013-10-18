#include <stdio.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <stdlib.h>

int  main(void)
{
    struct sysinfo *info;
    int code;
    info = (struct sysinfo*) malloc(sizeof(struct sysinfo));
    if (info == NULL) {
        printf("malloc failed");
    } else {
        code = sysinfo(info);
        if (code != 0) {
            printf("sysinfo failed");
        } else {
            printf("Hello User world\nUptime: %ld\nTotal RAM: %lu\nFree RAM: %lu\nProcess count: %d\nPage size: %d\n", info->uptime, info->totalram, info->freeram, info->procs, info->mem_unit);
        }
    }


    while(1){
        printf("hello, User Tobi\n");
        usleep(1000000);
    }
    return 0;
}





