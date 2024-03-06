#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
// #include <boost/process.hpp>

// namespace bp = boost::process;

static const size_t kDefaultBufSize = 4096;

// Adjust termios flags (e.g. clear ICANON for no input buffering)
// Constructor sets flags, and destructor restores, once object goes out of
// scope
class TermAttr {
 public:
  // Constructor saves existing termios flags, and sets/clears flags
  TermAttr(int fd, unsigned clear_flags = 0, unsigned set_flags = 0) : fd_{fd} {
    tcgetattr(fd, &old_term_);
    new_term_ = old_term_;
    new_term_.c_lflag &= ~clear_flags;
    new_term_.c_lflag |= set_flags;
    ::tcsetattr(fd, TCSANOW, &new_term_);
  };
  // Copy doesn't make sense as a unique object is required to track the given
  // terminal resource
  TermAttr(const TermAttr &) = delete;
  TermAttr &operator=(const TermAttr &) = delete;
  // Move is possibly useful
  TermAttr(TermAttr &&) = default;
  TermAttr &operator=(TermAttr &&) = default;
  // Destructor restores saved termios flags
  ~TermAttr() { ::tcsetattr(fd_, TCSANOW, &old_term_); };

 private:
  struct termios old_term_;
  struct termios new_term_;
  int fd_;
};

namespace {

// Spawn a thread to copy data from one FD to another
class Splicer {
 public:
  Splicer(int in_fd, int out_fd, std::string name = "",
          size_t buf_size = kDefaultBufSize)
      : in_fd_(in_fd),
        out_fd_(out_fd),
        name_(name),
        buf_size_(buf_size),
        sp_thread_(std::thread([this] { FdSplice(); })),
        has_pipe_(IsPipe(in_fd_) || IsPipe(out_fd_)) {}
  ~Splicer() { sp_thread_.detach(); }
  static void AllWait();

 private:
  Splicer() = delete;
  Splicer(const Splicer &) = delete;
  Splicer &operator=(const Splicer &) = delete;
  Splicer(Splicer &&) = delete;
  Splicer &operator=(Splicer &&) = delete;

  static std::mutex done_mutex_;
  static std::condition_variable done_condvar_;
  static bool all_done_;
  int in_fd_;
  int out_fd_;
  std::string name_;
  size_t buf_size_;
  std::thread sp_thread_;
  const bool has_pipe_;

  void FdSplice();
  static bool IsPipe(int fd);
};

std::mutex Splicer::done_mutex_{};
std::condition_variable Splicer::done_condvar_{};
bool Splicer::all_done_{false};

void Splicer::AllWait() {
  std::unique_lock<std::mutex> lk(done_mutex_);
  done_condvar_.wait(lk, [] { return all_done_; });
}

void Splicer::FdSplice() {
#ifdef SPLICE_F_MOVE
  if (has_pipe_) {
    ssize_t splice_size;

    while ((splice_size = ::splice(in_fd_, NULL, out_fd_, NULL, buf_size_,
                                 SPLICE_F_MOVE | SPLICE_F_MORE)) >= 0) {
    }
    if (splice_size == -1) {
      std::cerr << name_ << " splice(" << in_fd_ << ", " << out_fd_
                << ") error: " << errno << std::endl;
    }

  } else {
#endif
    ssize_t read_size;
    std::vector<uint8_t> buf(buf_size_);

    while ((read_size = ::read(in_fd_, buf.data(), buf_size_)) > 0) {
      ssize_t write_size = ::write(out_fd_, buf.data(), read_size);
      if (write_size == -1) {
        std::cerr << name_ << " write(" << out_fd_ << ") error: " << errno
                  << std::endl;
        break;
      }
    };
    if (read_size == -1) {
      std::cerr << name_ << " read(" << in_fd_ << ") error: " << errno
                << std::endl;
    }
#ifdef SPLICE_F_MOVE
  }
#endif

  {
    std::lock_guard<std::mutex> lk(done_mutex_);
    all_done_ = true;
  }
  done_condvar_.notify_one();
}

bool Splicer::IsPipe(int fd) {
  struct stat stat_buf;
  return ::fstat(fd, &stat_buf) && S_ISFIFO(stat_buf.st_mode);
}

int Child(int pt_fd, int err_fd, int argc, char *const *argv) {
  std::string child_dev(::ptsname(pt_fd) ?: "");

  ::close(pt_fd);
  ::setsid();

  int child_fd = ::open(child_dev.c_str(), O_RDWR | O_NOCTTY);
  if (child_fd < 0) {
    return -1;
  }

  if (::ioctl(child_fd, TIOCSCTTY, nullptr) == -1) {
    std::cerr << "ioctl(TIOCSCTTY) error: " << errno << std::endl;
    return -1;
  }

  ::dup2(child_fd, STDIN_FILENO);
  ::dup2(child_fd, STDOUT_FILENO);
  ::dup2(err_fd, STDERR_FILENO);
  ::close(child_fd);
  ::close(err_fd);

  if (::execvp(argv[1], &argv[1]) == -1) {
    std::cerr << "execvp() error: " << errno << std::endl;
    return -1;
  }

  return 0;
}

void Parent(int pt_fd, int err_fd) {
  TermAttr ta{STDIN_FILENO, ICANON};

  Splicer up{STDIN_FILENO, pt_fd, "up"};
  Splicer down{pt_fd, STDOUT_FILENO, "down"};
  Splicer down_err{err_fd, STDERR_FILENO, "down err"};

  Splicer::AllWait();
}

}  // namespace

int main(int argc, char *const *argv) {
  std::cout << "argc=" << argc << std::endl;
  for (int i = 0; i < argc; ++i) {
    std::cout << "argv[" << i << "]=\"" << argv[i] << "\"" << std::endl;
  }

  int pt_fd = ::posix_openpt(O_RDWR | O_NOCTTY);
  if (pt_fd == -1 || ::grantpt(pt_fd) == -1 || ::unlockpt(pt_fd) == -1) {
    return -1;
  }

  std::cout << "child device is: " << (::ptsname(pt_fd) ?: "") << std::endl;

  int err_fd[2];
  if (::pipe(err_fd) == -1) {
      std::cerr << "pipe() error: " << errno << std::endl;
      return -1;
  }

  int ret;
  pid_t pid = ::fork();
  switch (pid) {
    case -1:
      std::cerr << "fork() error: " << errno << std::endl;
      return -1;

    case 0:
      /* Child process */
      ret = Child(pt_fd, err_fd[1], argc, argv);
      if (-1 == ret) {
        return ret;
      }
      break;

    default:
      /* Parent process */
      std::cout << "Child pid " << pid << std::endl;
      Parent(pt_fd, err_fd[0]);
  }

  int status = 0;
  ::waitpid(pid, &status, 0);
  ::close(pt_fd);
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
