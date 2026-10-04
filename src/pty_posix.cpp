#if !defined(_WIN32)

#include "bropty/pty.h"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#elif defined(__linux__)
#include <pty.h>
#endif

namespace bropty {

class PtyPosix : public IPtyProcess {
public:
    PtyPosix();
    ~PtyPosix() override;

    bool spawn(const PtyConfig& config) override;
    size_t write(std::string_view data) override;
    size_t read(void* dst, size_t max_bytes) override;
    size_t read_timeout(void* dst, size_t max_bytes, std::chrono::milliseconds timeout) override;
    size_t read_nonblocking(void* dst, size_t max_bytes) override;
    bool resize(int cols, int rows) override;
    [[nodiscard]] bool is_running() const override;
    [[nodiscard]] int exit_code() const override;
    void terminate() override;
    void wait() override;

    [[nodiscard]] ByteRingBuffer& ring_buffer() override { return ring_buffer_; }

private:
    int master_fd_{-1};
    pid_t pid_{-1};

    ByteRingBuffer ring_buffer_{1024 * 1024};
    std::thread reader_thread_;
    std::atomic<bool> is_running_{false};
    int exit_code_{0};

    void cleanup();
    void reader_worker();
};

PtyPosix::PtyPosix() = default;

PtyPosix::~PtyPosix() {
    terminate();
    cleanup();
}

bool PtyPosix::spawn(const PtyConfig& config) {
    cleanup();

    struct winsize ws {};
    ws.ws_col = static_cast<unsigned short>(config.size.cols > 0 ? config.size.cols : 80);
    ws.ws_row = static_cast<unsigned short>(config.size.rows > 0 ? config.size.rows : 24);

    int slave_fd = -1;
    if (openpty(&master_fd_, &slave_fd, nullptr, nullptr, &ws) < 0) {
        return false;
    }

    pid_ = fork();
    if (pid_ < 0) {
        close(master_fd_);
        close(slave_fd);
        master_fd_ = -1;
        return false;
    }

    if (pid_ == 0) {
        // Child process
        close(master_fd_);

#if defined(__APPLE__) || defined(__linux__)
        login_tty(slave_fd);
#else
        setsid();
        ioctl(slave_fd, TIOCSCTTY, nullptr);
        dup2(slave_fd, STDIN_FILENO);
        dup2(slave_fd, STDOUT_FILENO);
        dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > STDERR_FILENO) close(slave_fd);
#endif

        // Set default environment
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        for (const auto& [k, v] : config.env) {
            setenv(k.c_str(), v.c_str(), 1);
        }

        if (!config.cwd.empty()) {
            if (chdir(config.cwd.c_str()) != 0) {
                // Ignore failure
            }
        }

        // Prepare exec args
        std::vector<char*> argv;
        std::string command = config.command.empty() ? "/bin/sh" : config.command;
        argv.push_back(const_cast<char*>(command.c_str()));
        for (const auto& arg : config.args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        execvp(command.c_str(), argv.data());
        _exit(127);
    }

    // Parent process
    close(slave_fd);
    is_running_ = true;

    ring_buffer_.reopen();
    reader_thread_ = std::thread(&PtyPosix::reader_worker, this);

    return true;
}

void PtyPosix::reader_worker() {
    uint8_t buffer[8192];
    while (is_running_) {
        ssize_t n = ::read(master_fd_, buffer, sizeof(buffer));
        if (n <= 0) {
            break;
        }
        ring_buffer_.write(buffer, static_cast<size_t>(n));
    }
    ring_buffer_.close();
    is_running_ = false;
}

size_t PtyPosix::write(std::string_view data) {
    if (master_fd_ < 0 || data.empty()) return 0;
    ssize_t n = ::write(master_fd_, data.data(), data.size());
    return n > 0 ? static_cast<size_t>(n) : 0;
}

size_t PtyPosix::read(void* dst, size_t max_bytes) {
    return ring_buffer_.read(dst, max_bytes);
}

size_t PtyPosix::read_timeout(void* dst, size_t max_bytes, std::chrono::milliseconds timeout) {
    return ring_buffer_.read_timeout(dst, max_bytes, timeout);
}

size_t PtyPosix::read_nonblocking(void* dst, size_t max_bytes) {
    return ring_buffer_.read_nonblocking(dst, max_bytes);
}

bool PtyPosix::resize(int cols, int rows) {
    if (master_fd_ < 0) return false;
    struct winsize ws {};
    ws.ws_col = static_cast<unsigned short>(cols > 0 ? cols : 80);
    ws.ws_row = static_cast<unsigned short>(rows > 0 ? rows : 24);
    return ioctl(master_fd_, TIOCSWINSZ, &ws) >= 0;
}

bool PtyPosix::is_running() const {
    if (!is_running_ || pid_ <= 0) return false;
    int status = 0;
    pid_t result = waitpid(pid_, &status, WNOHANG);
    return result == 0;
}

int PtyPosix::exit_code() const {
    if (pid_ > 0) {
        int status = 0;
        pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        }
    }
    return exit_code_;
}

void PtyPosix::terminate() {
    is_running_ = false;

    if (pid_ > 0) {
        kill(pid_, SIGTERM);
    }
    if (master_fd_ >= 0) {
        close(master_fd_);
        master_fd_ = -1;
    }
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
    if (pid_ > 0) {
        int status = 0;
        waitpid(pid_, &status, 0);
        if (WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
        pid_ = -1;
    }
}

void PtyPosix::wait() {
    if (pid_ > 0) {
        int status = 0;
        waitpid(pid_, &status, 0);
        if (WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
        pid_ = -1;
    }
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
}

void PtyPosix::cleanup() {
    terminate();
}

std::unique_ptr<IPtyProcess> create_pty_posix() {
    return std::make_unique<PtyPosix>();
}

} // namespace bropty

#endif // !_WIN32
