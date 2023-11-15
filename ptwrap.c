#define _XOPEN_SOURCE
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>

static int eof = 0;

void
sig_chld (int signum) {
    eof = 1;
    return;
}

int
main (int argc, char * const *argv)
{
    int i;

    printf("argc=%d\n", argc);
    for (i = 0; i < argc; i++) {
        printf("argv[%d]=\"%s\"\n", i, argv[i]);
    }

    int masterfd;
    char *slavedevice;

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

    printf("slave device is: %s\n", slavedevice);

    pid_t pid;

    pid = fork();
    switch (pid) {
    case -1:
        perror("fork()");
        return -1;

    case 0: {
        /* Child process */

        int slavefd;

        close(masterfd);
        setsid();

        slavefd = open(slavedevice, O_RDWR | O_NOCTTY);
        if (slavefd < 0) {
            return -1;
        }

        if (ioctl(slavefd, TIOCSCTTY, NULL) == -1)
        {
            perror("ioctl(TIOCSCTTY)");
            return -1;
        }

        dup2(slavefd, STDIN_FILENO);
        dup2(slavefd, STDOUT_FILENO);
        dup2(slavefd, STDERR_FILENO);
        close(slavefd);

        if (execvp(argv[1], &argv[1]) == -1) {
            perror("execvp()");
            return -1;
        }
        break;
    }

    default:
        /* Parent process */
        printf("Child pid %d\n", pid);

        int max_fd = masterfd + 1;
        fd_set readfds;
        int sel_fds;
        ssize_t sz;
        char buf[4096];
        int flags;
        struct sigaction sa;

        sa.sa_handler = sig_chld;
        sigaction(SIGCHLD, &sa, 0);

        flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

        flags = fcntl(masterfd, F_GETFL, 0);
        fcntl(masterfd, F_SETFL, flags | O_NONBLOCK);

        while (!eof) {
            FD_ZERO(&readfds);

            FD_SET(STDIN_FILENO, &readfds);
            FD_SET(masterfd, &readfds);

            sel_fds = select(max_fd, &readfds, NULL, NULL, NULL);
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

            if (FD_ISSET(STDIN_FILENO, &readfds)) {
                do {
                    sz = read(STDIN_FILENO, buf, sizeof(buf));
                    if (sz == -1) {
                        if (errno == EWOULDBLOCK) {
                            break;
                        }
                        perror("read(STDIN_FILENO)");
                        return -1;
                    }
                    if (sz == 0) {
                        eof = 1;
                    }
                    if (write(masterfd, buf, sz) == -1) {
                        perror("write(masterfd)");
                        return -1;
                    }
                } while (sz > 0);
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

    }

    int status;

    if (waitpid(pid, &status, 0) == -1) {
        perror("waitpid()");
        return -1;
    }
    printf("Child status %d\n", WEXITSTATUS(status));

    close(masterfd);

    return 0;
}
