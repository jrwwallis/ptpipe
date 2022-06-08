#define _XOPEN_SOURCE
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>


int
main (int argc, char * const *argv)
{
    int eof = 0;
    int masterfd;
    char *slavedevice;
    int fd_width;
    fd_set readfds;
    int sel_fds;
    ssize_t sz;
    char buf[4096];
    int flags;

    masterfd = posix_openpt(O_RDWR | O_NOCTTY);

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

    fd_width = masterfd + 1;

    printf("slave device is: %s\n", slavedevice);

    flags = fcntl(masterfd, F_GETFL, 0);
    fcntl(masterfd, F_SETFL, flags | O_NONBLOCK);

    while (!eof) {
        FD_ZERO(&readfds);

        FD_SET(masterfd, &readfds);

        sel_fds = select(fd_width, &readfds, NULL, NULL, NULL);
        switch (sel_fds) {
        case -1:
            if (errno == EINTR) {
                eof = 1;
                break;
            }
            perror("select()");
            return -1;

        case 0:
            continue;

        }

        if (FD_ISSET(masterfd, &readfds)) {
            do {
                sz = read(masterfd, buf, sizeof(buf));
                if (sz == -1) {
                    if (errno == EWOULDBLOCK) {
                        break;
                    }
                    if (errno == EIO) {
                        eof = 1;
                        break;
                    }
                    perror("read(masterfd)");
                    return -1;
                }
                if (sz == 0) {
                    eof = 1;
                }
                if (write(STDOUT_FILENO, buf, sz) == -1) {
                    perror("write(STDOUT_FILENO)");
                    return -1;
                }
            } while (sz > 0);
        }
    }

    close(masterfd);

    return 0;
}
