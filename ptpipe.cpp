#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>

#include <iostream>
#include <thread>
#include <mutex>
//#include <boost/process.hpp>

//namespace bp = boost::process;

static constexpr size_t DEFAULT_BUFSZ = 4096;

class TermAttr
{
public:
    explicit TermAttr(const int fd, const int clear_mask = 0, const int set_mask = 0) : fd{fd}
    {
        tcgetattr(fd, &oldt);
        newt = oldt;
        newt.c_lflag &= ~clear_mask;
        newt.c_lflag |= set_mask;
        tcsetattr(fd, TCSANOW, &newt);
    };
    // Not copyable or movable
    TermAttr(const TermAttr&) = delete;
    TermAttr& operator=(const TermAttr&) = delete;
    ~TermAttr()
    {
        tcsetattr(fd, TCSANOW, &oldt);
    };

private:
    struct termios oldt;
    struct termios newt;
    int fd;
};

namespace {

class Splicer
{
public:
    Splicer(const int in_fd, const int out_fd, const size_t bufsz = DEFAULT_BUFSZ):
        in_fd(in_fd), out_fd(out_fd), bufsz(bufsz), sp_thread(std::thread( [this] {fd_splice();})) {
    }
    ~Splicer() {
        sp_thread.detach();
    }
    static inline void all_wait() {
        std::unique_lock<std::mutex> lk(done_mutex);
        done_condvar.wait(lk, []{return all_done;});
    }

private:
    std::thread sp_thread;
    inline static std::mutex done_mutex{};
    inline static std::condition_variable done_condvar{};
    inline static bool all_done{false};
    const int in_fd;
    const int out_fd;
    const size_t bufsz;

    void fd_splice()
    {
        ssize_t sz = 0;
        std::vector<uint8_t> buf (bufsz);

        while ((sz = read(in_fd, buf.data(), bufsz)) > 0) {
            sz = write(out_fd, buf.data(), sz);
            if (sz == -1)
            {
                std::cerr << "write(masterfd) error: " << errno << std::endl;
                sz = 0;
                break;
            }
        }
        if (sz == -1) {
            std::cerr << "read(STDIN_FILENO) error: " << errno << std::endl;
        }  

        {
            std::lock_guard<std::mutex> lk(done_mutex);
            all_done = true;
        }
        done_condvar.notify_one();
    }
};


int child(const int &masterfd, const int argc, char *const *argv)
{
    const char *slavedevice = ptsname(masterfd);
    if (slavedevice == nullptr)
    {
        std::cerr << "ptsname() error: " << errno << std::endl;
        return -1;
    }

    close(masterfd);
    setsid();

    const int slavefd = open(slavedevice, O_RDWR | O_NOCTTY);
    if (slavefd < 0)
    {
        return -1;
    }

    if (ioctl(slavefd, TIOCSCTTY, nullptr) == -1)
    {
        std::cerr << "ioctl(TIOCSCTTY) error: " << errno << std::endl;
        return -1;
    }

    dup2(slavefd, STDIN_FILENO);
    dup2(slavefd, STDOUT_FILENO);
    dup2(slavefd, STDERR_FILENO);
    close(slavefd);

    if (execvp(argv[1], &argv[1]) == -1)
    {
        std::cerr << "execvp() error: " << errno << std::endl;
        return -1;
    }

    return 0;
}

void parent(const int &masterfd)
{
    TermAttr ta{STDIN_FILENO, ICANON};

    Splicer up{STDIN_FILENO, masterfd};
    Splicer down{masterfd, STDOUT_FILENO};

    Splicer::all_wait();
}

} // namespace

int main(const int argc, char *const *argv)
{
    std::cout << "argc=" << argc << std::endl;
    for (int i = 0; i < argc; i++)
    {
        std::cout << "argv[" << i << "]=\"" << argv[i] << "\"" << std::endl;
    }

    const int masterfd = posix_openpt(O_RDWR | O_NOCTTY);

    if (masterfd == -1 || grantpt(masterfd) == -1 || unlockpt(masterfd) == -1)
    {
        return -1;
    }

    const char *slavedevice = ptsname(masterfd);
    if (slavedevice == nullptr)
    {
        std::cerr << "ptsname() error: " << errno << std::endl;
        return -1;
    }

    std::cout << "slave device is: " << slavedevice << std::endl;

    pid_t pid = fork();
    switch (pid)
    {
    case -1:
        std::cerr << "fork() error: " << errno << std::endl;
        return -1;

    case 0: {
        /* Child process */
        const int ret = child(masterfd, argc, argv);
        if (-1 == ret)
        {
            return ret;
        }
        break;
    }

    default:
        /* Parent process */
        std::cout << "Child pid " << pid << std::endl;

        parent(masterfd);
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
