#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "bropty/pty.h"
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace bropty {

class PtyWin : public IPtyProcess {
public:
    PtyWin();
    ~PtyWin() override;

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
    HPCON hpc_{INVALID_HANDLE_VALUE};
    HANDLE process_{INVALID_HANDLE_VALUE};
    HANDLE thread_{INVALID_HANDLE_VALUE};
    HANDLE pipe_in_write_{INVALID_HANDLE_VALUE};
    HANDLE pipe_out_read_{INVALID_HANDLE_VALUE};

    ByteRingBuffer ring_buffer_{1024 * 1024};
    std::thread reader_thread_;
    std::atomic<bool> is_running_{false};
    int exit_code_{0};

    void cleanup();
    void reader_worker();
};

PtyWin::PtyWin() = default;

PtyWin::~PtyWin() {
    terminate();
    cleanup();
}

static std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) return L"";
    int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (count <= 0) return L"";
    std::wstring out(count, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), count);
    return out;
}

bool PtyWin::spawn(const PtyConfig& config) {
    cleanup();

    HANDLE pipe_in_read = INVALID_HANDLE_VALUE;
    HANDLE pipe_out_write = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&pipe_in_read, &pipe_in_write_, nullptr, 0)) {
        return false;
    }
    if (!CreatePipe(&pipe_out_read_, &pipe_out_write, nullptr, 0)) {
        CloseHandle(pipe_in_read);
        CloseHandle(pipe_in_write_);
        pipe_in_write_ = INVALID_HANDLE_VALUE;
        return false;
    }

    COORD coord{};
    coord.X = static_cast<SHORT>(config.size.cols > 0 ? config.size.cols : 80);
    coord.Y = static_cast<SHORT>(config.size.rows > 0 ? config.size.rows : 24);

    HRESULT hr = CreatePseudoConsole(coord, pipe_in_read, pipe_out_write, 0, &hpc_);
    if (FAILED(hr)) {
        CloseHandle(pipe_in_read);
        CloseHandle(pipe_out_write);
        CloseHandle(pipe_in_write_);
        CloseHandle(pipe_out_read_);
        pipe_in_write_ = INVALID_HANDLE_VALUE;
        pipe_out_read_ = INVALID_HANDLE_VALUE;
        return false;
    }

    SIZE_T bytes_required = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes_required);
    auto* attr_list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, bytes_required));

    if (!attr_list || !InitializeProcThreadAttributeList(attr_list, 1, 0, &bytes_required)) {
        if (attr_list) HeapFree(GetProcessHeap(), 0, attr_list);
        CloseHandle(pipe_in_read);
        CloseHandle(pipe_out_write);
        cleanup();
        return false;
    }

    if (!UpdateProcThreadAttribute(attr_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc_, sizeof(HPCON), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attr_list);
        HeapFree(GetProcessHeap(), 0, attr_list);
        CloseHandle(pipe_in_read);
        CloseHandle(pipe_out_write);
        cleanup();
        return false;
    }

    STARTUPINFOEXW si_ex{};
    si_ex.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    si_ex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si_ex.lpAttributeList = attr_list;

    // Build command line
    std::wstring cmdline;
    if (config.command.empty()) {
        cmdline = L"cmd.exe";
    } else {
        cmdline = utf8_to_wide(config.command);
    }
    for (const auto& arg : config.args) {
        std::wstring warg = utf8_to_wide(arg);
        cmdline += L" ";
        if (warg.find(L' ') != std::wstring::npos || warg.find(L'\t') != std::wstring::npos) {
            cmdline += L"\"" + warg + L"\"";
        } else {
            cmdline += warg;
        }
    }

    std::wstring cwd;
    if (!config.cwd.empty()) {
        cwd = utf8_to_wide(config.cwd);
    }

    PROCESS_INFORMATION pi{};
    BOOL success = CreateProcessW(
        nullptr,
        cmdline.data(),
        nullptr,
        nullptr,
        FALSE,
        EXTENDED_STARTUPINFO_PRESENT,
        nullptr,
        cwd.empty() ? nullptr : cwd.c_str(),
        &si_ex.StartupInfo,
        &pi);

    DeleteProcThreadAttributeList(attr_list);
    HeapFree(GetProcessHeap(), 0, attr_list);

    // Close the PseudoConsole side of the pipes now that process is created
    CloseHandle(pipe_in_read);
    CloseHandle(pipe_out_write);

    if (!success) {
        cleanup();
        return false;
    }

    process_ = pi.hProcess;
    thread_ = pi.hThread;
    is_running_ = true;

    // Start reader thread
    ring_buffer_.reopen();
    reader_thread_ = std::thread(&PtyWin::reader_worker, this);

    return true;
}

void PtyWin::reader_worker() {
    uint8_t buffer[8192];
    DWORD bytes_read = 0;

    while (true) {
        BOOL ok = ReadFile(pipe_out_read_, buffer, sizeof(buffer), &bytes_read, nullptr);
        if (!ok || bytes_read == 0) {
            break;
        }
        ring_buffer_.write(buffer, bytes_read);
    }

    ring_buffer_.close();
    is_running_ = false;
}

size_t PtyWin::write(std::string_view data) {
    if (pipe_in_write_ == INVALID_HANDLE_VALUE || data.empty()) {
        return 0;
    }
    DWORD bytes_written = 0;
    if (!WriteFile(pipe_in_write_, data.data(), static_cast<DWORD>(data.size()), &bytes_written, nullptr)) {
        return 0;
    }
    return bytes_written;
}

size_t PtyWin::read(void* dst, size_t max_bytes) {
    return ring_buffer_.read(dst, max_bytes);
}

size_t PtyWin::read_timeout(void* dst, size_t max_bytes, std::chrono::milliseconds timeout) {
    return ring_buffer_.read_timeout(dst, max_bytes, timeout);
}

size_t PtyWin::read_nonblocking(void* dst, size_t max_bytes) {
    return ring_buffer_.read_nonblocking(dst, max_bytes);
}

bool PtyWin::resize(int cols, int rows) {
    if (hpc_ == INVALID_HANDLE_VALUE) return false;
    COORD coord{};
    coord.X = static_cast<SHORT>(cols > 0 ? cols : 80);
    coord.Y = static_cast<SHORT>(rows > 0 ? rows : 24);
    return SUCCEEDED(ResizePseudoConsole(hpc_, coord));
}

bool PtyWin::is_running() const {
    if (!is_running_) return false;
    if (process_ != INVALID_HANDLE_VALUE) {
        DWORD exit = 0;
        if (GetExitCodeProcess(process_, &exit) && exit != STILL_ACTIVE) {
            return false;
        }
    }
    return true;
}

int PtyWin::exit_code() const {
    if (process_ != INVALID_HANDLE_VALUE) {
        DWORD exit = 0;
        if (GetExitCodeProcess(process_, &exit)) {
            return static_cast<int>(exit);
        }
    }
    return exit_code_;
}

void PtyWin::terminate() {
    is_running_ = false;

    if (hpc_ != INVALID_HANDLE_VALUE) {
        ClosePseudoConsole(hpc_);
        hpc_ = INVALID_HANDLE_VALUE;
    }

    if (pipe_in_write_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_in_write_);
        pipe_in_write_ = INVALID_HANDLE_VALUE;
    }

    if (process_ != INVALID_HANDLE_VALUE) {
        DWORD exit = 0;
        if (GetExitCodeProcess(process_, &exit) && exit == STILL_ACTIVE) {
            TerminateProcess(process_, 1);
        }
    }

    if (pipe_out_read_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_out_read_);
        pipe_out_read_ = INVALID_HANDLE_VALUE;
    }

    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
}

void PtyWin::wait() {
    if (process_ != INVALID_HANDLE_VALUE) {
        WaitForSingleObject(process_, INFINITE);
        DWORD exit = 0;
        if (GetExitCodeProcess(process_, &exit)) {
            exit_code_ = static_cast<int>(exit);
        }
    }
    if (hpc_ != INVALID_HANDLE_VALUE) {
        ClosePseudoConsole(hpc_);
        hpc_ = INVALID_HANDLE_VALUE;
    }
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
}

void PtyWin::cleanup() {
    terminate();

    if (thread_ != INVALID_HANDLE_VALUE) {
        CloseHandle(thread_);
        thread_ = INVALID_HANDLE_VALUE;
    }
    if (process_ != INVALID_HANDLE_VALUE) {
        CloseHandle(process_);
        process_ = INVALID_HANDLE_VALUE;
    }
}

std::unique_ptr<IPtyProcess> create_pty_win() {
    return std::make_unique<PtyWin>();
}

} // namespace bropty

#endif // _WIN32
