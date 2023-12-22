#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#include <iostream>
#include <thread>
#include <mutex>

// TODO pass list of set/unset flags to constructor
class TermAttr
{
public:
    TermAttr(int fd) : fd{fd}
    {
        tcgetattr(fd, &oldt);
        newt = oldt;
        newt.c_lflag &= ~(ICANON);
        tcsetattr(fd, TCSANOW, &newt);
    };
    ~TermAttr()
    {
        tcsetattr(fd, TCSANOW, &oldt);
    };

private:
    struct termios oldt, newt;
    int fd;
};

class Splicer
{
private:
    std::thread sp_thread;
    inline static std::mutex done_mutex{};
    inline static std::condition_variable done_condvar{};
    inline static bool all_done{false};
    int in_fd;
    int out_fd;
    size_t bufsz;

    void fd_splice()
    {
        ssize_t sz;
        std::vector<uint8_t> buf (bufsz);

        pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
        do
        {
            sz = read(in_fd, buf.data(), bufsz);
            if (sz == -1)
            {
                //printf("read(%d), dirn=%s, errno=%d\n", in_fd, fd_splice_args->dirn, errno);
                perror("read(STDIN_FILENO)");
                break;
            }
            if (sz == 0)
            {
                break;
            }
            sz = write(out_fd, buf.data(), sz);
            if (sz == -1)
            {
                perror("write(masterfd)");
                break;
            }
        } while (sz > 0);

        {
            std::lock_guard<std::mutex> lk(done_mutex);
            all_done = true;
        }
        done_condvar.notify_one();
    }

public:
    Splicer(int in_fd, int out_fd, size_t bufsz = 4096):in_fd(in_fd), out_fd(out_fd), bufsz(bufsz), sp_thread(std::thread( [this] {fd_splice();})) {
    }
    static inline void all_wait() {
        std::unique_lock<std::mutex> lk(done_mutex);
        done_condvar.wait(lk, []{return all_done;});

    }
    void join() {
        sp_thread.join();
    }
};


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

int parent(int masterfd)
{
    TermAttr ta{STDIN_FILENO};

    Splicer up{STDIN_FILENO, masterfd};
    Splicer down{masterfd, STDOUT_FILENO};

    Splicer::all_wait();

    close(masterfd);
    up.join();
    down.join();

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
    if (WIFEXITED(status))
    {
        return WEXITSTATUS(status);
    }
    else if (WIFSIGNALED(status))
    {
        return 128 + (WTERMSIG(status));
    }
    else if (WIFSTOPPED(status))
    {
        return 128 + (WSTOPSIG(status));
    }
    else
    {
        return 1;
    }
}
