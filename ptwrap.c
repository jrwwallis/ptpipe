#define _XOPEN_SOURCE
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>

int
main (int argc, const char **argv)
{
    int i;

    printf("argc=%d\n", argc);
    for (i = 0; i < argc; i++) {
        printf("argv[%d]=\"%s\"\n", i, argv[i]);
    }

    int masterfd, slavefd;
    char *slavedevice;

    masterfd = posix_openpt(O_RDWR|O_NOCTTY);

    if (masterfd == -1
        || grantpt(masterfd) == -1
        || unlockpt(masterfd) == -1) {
        return -1;
    }

    slavedevice = ptsname(masterfd);
    if (slavedevice == NULL) {
        perror("ptsname()");
        return -1;
    }

    printf("slave device is: %s\n", slavedevice);

    slavefd = open(slavedevice, O_RDWR|O_NOCTTY);
    if (slavefd < 0) {
        return -1;
    }

    return 0;
}
