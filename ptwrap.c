#define _XOPEN_SOURCE
#define _GNU_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h>
#include <assert.h>

static pthread_mutex_t done_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t done_condvar = PTHREAD_COND_INITIALIZER;
static int done = 0;

void sig_chld(int signum)
{
    pthread_mutex_lock(&done_mutex);
    done = 1;
    pthread_cond_signal(&done_condvar);
    pthread_mutex_unlock(&done_mutex);
    return;
}

int child(int masterfd, int argc, char *const *argv)
{
    int slavefd;
    char *slavedevice;

    slavedevice = ptsname(masterfd);
    if (slavedevice == NULL)
    {
        perror("ptsname()");
        return -1;
    }

    close(masterfd);
    setsid();

    slavefd = open(slavedevice, O_RDWR | O_NOCTTY);
    if (slavefd < 0)
    {
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

    if (execvp(argv[1], &argv[1]) == -1)
    {
        perror("execvp()");
        return -1;
    }

    return 0;
}

struct fd_splice_args_s
{
    const char *dirn;
    int in_fd;
    int out_fd;
    ssize_t bufsz;
};

void *
fd_splice(void *args)
{
    struct fd_splice_args_s *fd_splice_args = (struct fd_splice_args_s *)args;
    ssize_t sz;
    uint8_t *buf = (uint8_t *)malloc(fd_splice_args->bufsz);
    assert(buf != NULL);

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    do {
        sz = read(fd_splice_args->in_fd, buf, fd_splice_args->bufsz);
        if (sz == -1) {
            printf("read(%d), dirn=%s, errno=%d\n", fd_splice_args->in_fd, fd_splice_args->dirn, errno);
            perror("read(STDIN_FILENO)");
            break;
        }
        if (sz == 0) {
            break;
        }
        sz = write(fd_splice_args->out_fd, buf, sz);
        if (sz == -1) {
            perror("write(masterfd)");
            break;
        }
    } while (sz > 0);

    free(buf);

    pthread_mutex_lock(&done_mutex);
    done = 1;
    pthread_cond_signal(&done_condvar);
    pthread_mutex_unlock(&done_mutex);

    return NULL;
}

int parent(int masterfd)
{
    struct sigaction sa;
    pthread_t up, down;

    struct fd_splice_args_s up_args = {
        .dirn = "up",
        .in_fd = STDIN_FILENO,
        .out_fd = masterfd,
        .bufsz = 4096,
    };
    struct fd_splice_args_s down_args = {
        .dirn = "down",
        .in_fd = masterfd,
        .out_fd = STDOUT_FILENO,
        .bufsz = 4096,
    };

    struct termios oldt, newt;

    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    pthread_create(&down, NULL, fd_splice, &down_args);
    pthread_create(&up, NULL, fd_splice, &up_args);

    sa.sa_handler = sig_chld;
    sigaction(SIGCHLD, &sa, 0);

    pthread_mutex_lock(&done_mutex);
    while (!done) {
        pthread_cond_wait(&done_condvar, &done_mutex);
    }
    pthread_mutex_unlock(&done_mutex);

    pthread_cancel(down);
    pthread_cancel(up);

    pthread_join(down, NULL);
    pthread_join(up, NULL);

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

    return 0;
}

int main(int argc, char *const *argv)
{
    int i;

    printf("argc=%d\n", argc);
    for (i = 0; i < argc; i++)
    {
        printf("argv[%d]=\"%s\"\n", i, argv[i]);
    }

    int masterfd;
    char *slavedevice;

    masterfd = posix_openpt(O_RDWR | O_NOCTTY);

    if (masterfd == -1 || grantpt(masterfd) == -1 || unlockpt(masterfd) == -1)
    {
        return -1;
    }

    slavedevice = ptsname(masterfd);
    if (slavedevice == NULL)
    {
        perror("ptsname()");
        return -1;
    }

    printf("slave device is: %s\n", slavedevice);

    pid_t pid;

    int ret;
    pid = fork();
    switch (pid)
    {
    case -1:
        perror("fork()");
        return -1;

    case 0:
        /* Child process */
        ret = child(masterfd, argc, argv);
        if (-1 == ret)
        {
            return ret;
        }
        break;

    default:
        /* Parent process */
        printf("Child pid %d\n", pid);

        ret = parent(masterfd);
        if (-1 == ret)
        {
            return ret;
        }
    }

    int status;

    waitpid(pid, &status, 0);
    close(masterfd);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        return 128 + (WTERMSIG(status));
    } else if (WIFSTOPPED(status)) {
        return 128 + (WSTOPSIG(status));
    } else {
        return 1;
    }
}
