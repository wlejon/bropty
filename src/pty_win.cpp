// ConPTY backend.
//
// Threads: a reader (output pipe -> ring), a writer (input queue -> input
// pipe) and a waiter (process exit -> exit code, then close the console so
// conhost flushes and the output pipe reaches EOF).
//
// Teardown hazards this is built around:
//  - ClosePseudoConsole waits (before Windows 11 24H2) for conhost to exit,
//    and conhost blocks writing its last output if nobody drains the pipe. The
//    reader therefore keeps draining (discarding once the ring is closed),
//    and the close itself runs on a helper thread with a bounded wait.
//  - Synchronous ReadFile / WriteFile on pipes do not return when the handle
//    is closed from another thread: blocked I/O is cancelled with
//    CancelSynchronousIo until the thread has left.
//  - A child that ignores CTRL_CLOSE_EVENT is killed after
//    PtyConfig::terminate_grace. Under ProcessTree::Tree the child is created
//    inside a kill-on-close job object, so the kill takes the whole tree --
//    including grandchildren that detached from the console (DETACHED_PROCESS,
//    FreeConsole, GUI programs) -- except those created with
//    CREATE_BREAKAWAY_FROM_JOB, the explicit opt-out the job allows.
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "pty_base.h"

#include <algorithm>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace bropty {

namespace {

std::wstring widen(std::string_view s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string system_error(const char* what, DWORD code = GetLastError()) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::string text = msg ? narrow(msg) : std::string();
    if (msg) LocalFree(msg);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return std::string(what) + ": " + text + " (" + std::to_string(code) + ")";
}

std::vector<std::string> current_environment() {
    std::vector<std::string> out;
    wchar_t* block = GetEnvironmentStringsW();
    if (!block) return out;
    for (const wchar_t* p = block; *p; p += wcslen(p) + 1) out.push_back(narrow(p));
    FreeEnvironmentStringsW(block);
    return out;
}

std::wstring environment_block(const PtyConfig& config) {
    std::wstring block;
    for (const std::string& e : pty_detail::build_environment(current_environment(), config, true)) {
        block += widen(e);
        block.push_back(L'\0');
    }
    if (block.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

COORD coord_of(const PtySize& s) {
    COORD c;
    c.X = SHORT(std::clamp(s.cols, 1, 32767));
    c.Y = SHORT(std::clamp(s.rows, 1, 32767));
    return c;
}

struct Handle {
    HANDLE h{nullptr};
    Handle() = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset(HANDLE v = nullptr) {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
        h = v;
    }
    explicit operator bool() const { return h && h != INVALID_HANDLE_VALUE; }
};

// Leave a thread blocked in synchronous pipe I/O: cancel until it is gone.
void cancel_and_join(std::thread& t, const std::atomic<bool>& done) {
    if (!t.joinable()) return;
    HANDLE h = static_cast<HANDLE>(t.native_handle());
    while (!done.load()) {
        CancelSynchronousIo(h);
        if (WaitForSingleObject(h, 10) == WAIT_OBJECT_0) break;
    }
    t.join();
}

} // namespace

class PtyWin final : public pty_detail::PtyBase {
public:
    PtyWin() = default;
    ~PtyWin() override { terminate(); }

    bool spawn(const PtyConfig& config) override;
    bool resize(const PtySize& size) override;
    int64_t pid() const override { return pid_; }
    void terminate() override;

private:
    void reader_main();
    void writer_main();
    void waiter_main();
    void close_console();

    std::mutex console_mu_;
    HPCON hpc_{nullptr};
    Handle process_;
    Handle job_;  // ProcessTree::Tree
    Handle in_write_;
    Handle out_read_;
    Handle stop_event_;
    int64_t pid_{0};

    std::thread reader_, writer_, waiter_;
    std::atomic<bool> reader_done_{false}, writer_done_{false};
    std::once_flag teardown_;
};

bool PtyWin::spawn(const PtyConfig& config) {
    if (!begin_spawn(config)) return false;

    Handle in_read, out_write;
    if (!CreatePipe(&in_read.h, &in_write_.h, nullptr, 0)) return fail(system_error("CreatePipe"));
    if (!CreatePipe(&out_read_.h, &out_write.h, nullptr, 0)) return fail(system_error("CreatePipe"));

    HRESULT hr = CreatePseudoConsole(coord_of(config.size), in_read.h, out_write.h, 0, &hpc_);
    if (FAILED(hr)) {
        hpc_ = nullptr;
        return fail(system_error("CreatePseudoConsole", DWORD(hr)));
    }

    // The process tree lives in a job: kill-on-close (a crashed host leaks
    // nothing), breakaway permitted only to processes that ask for it.
    if (config.process_tree == PtyConfig::ProcessTree::Tree) {
        job_.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job_) {
            close_console();
            return fail(system_error("CreateJobObject"));
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        if (!SetInformationJobObject(job_.h, JobObjectExtendedLimitInformation, &lim, sizeof lim)) {
            close_console();
            return fail(system_error("SetInformationJobObject"));
        }
    }
    const DWORD nattrs = job_ ? 2 : 1;
    SIZE_T attr_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, nattrs, 0, &attr_bytes);
    std::vector<unsigned char> attr_storage(attr_bytes);
    auto* attrs = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attr_storage.data());
    if (!InitializeProcThreadAttributeList(attrs, nattrs, 0, &attr_bytes)) {
        close_console();
        return fail(system_error("InitializeProcThreadAttributeList"));
    }
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc_, sizeof(HPCON), nullptr,
                                   nullptr)) {
        DeleteProcThreadAttributeList(attrs);
        close_console();
        return fail(system_error("UpdateProcThreadAttribute"));
    }
    // Created inside the job, so not even its first instruction runs outside
    // (no window in which it could spawn something the job misses).
    HANDLE job_list[1] = {job_.h};
    if (job_ && !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, job_list, sizeof job_list,
                                           nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attrs);
        close_console();
        return fail(system_error("UpdateProcThreadAttribute(JOB_LIST)"));
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    // Null standard handles + STARTF_USESTDHANDLES: the child must not
    // inherit the host's (possibly redirected) stdio instead of the console.
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.lpAttributeList = attrs;

    std::string line = config.windows_command_line;
    if (line.empty()) {
        std::string command = config.command;
        if (command.empty()) {
            wchar_t comspec[MAX_PATH];
            DWORD n = GetEnvironmentVariableW(L"COMSPEC", comspec, MAX_PATH);
            command = (n > 0 && n < MAX_PATH) ? narrow(std::wstring_view(comspec, n)) : "cmd.exe";
        }
        line = pty_detail::windows_command_line(command, config.args);
    }
    std::wstring wline = widen(line);
    std::wstring wcwd = widen(config.cwd);
    std::wstring env = environment_block(config);

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, wline.data(), nullptr, nullptr, FALSE,
                             EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, env.data(),
                             wcwd.empty() ? nullptr : wcwd.c_str(), &si.StartupInfo, &pi);
    DWORD err = GetLastError();
    DeleteProcThreadAttributeList(attrs);
    if (!ok) {
        close_console();
        return fail(system_error("CreateProcess", err));
    }
    CloseHandle(pi.hThread);
    process_.reset(pi.hProcess);
    pid_ = int64_t(pi.dwProcessId);
    // conhost holds its own duplicates of the pipe ends it uses.
    in_read.reset();
    out_write.reset();

    stop_event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    spawned_ = true;
    reader_ = std::thread(&PtyWin::reader_main, this);
    writer_ = std::thread(&PtyWin::writer_main, this);
    waiter_ = std::thread(&PtyWin::waiter_main, this);
    return true;
}

void PtyWin::reader_main() {
    std::vector<char> buf(64 * 1024);
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(out_read_.h, buf.data(), DWORD(buf.size()), &n, nullptr) || n == 0) break;
        deliver_output(buf.data(), n);
    }
    output_finished();
    reader_done_ = true;
}

void PtyWin::writer_main() {
    std::string chunk;
    while (take_input(chunk, 64 * 1024)) {
        size_t off = 0;
        while (off < chunk.size()) {
            DWORD n = 0;
            if (!WriteFile(in_write_.h, chunk.data() + off, DWORD(chunk.size() - off), &n, nullptr)) {
                stop_input();  // console gone (or cancelled by teardown)
                break;
            }
            off += n;
        }
        chunk.clear();
    }
    writer_done_ = true;
}

void PtyWin::waiter_main() {
    HANDLE hs[2] = {process_.h, stop_event_.h};
    if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) != WAIT_OBJECT_0) return;
    DWORD code = 0;
    GetExitCodeProcess(process_.h, &code);
    set_exited(int(code));
    // The console outlives its client; closing it makes conhost flush what
    // the child wrote and then end the output pipe (EOF for the reader).
    close_console();
}

void PtyWin::close_console() {
    HPCON h;
    {
        std::lock_guard<std::mutex> lock(console_mu_);
        h = hpc_;
        hpc_ = nullptr;
    }
    if (!h) return;
    // Bounded: should ClosePseudoConsole ever hang, the helper is abandoned.
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> f = done->get_future();
    std::thread([h, done] {
        ClosePseudoConsole(h);
        done->set_value();
    }).detach();
    f.wait_for(std::chrono::seconds(5));
}

bool PtyWin::resize(const PtySize& size) {
    std::lock_guard<std::mutex> lock(console_mu_);
    if (!hpc_) return false;
    return SUCCEEDED(ResizePseudoConsole(hpc_, coord_of(size)));
}

void PtyWin::terminate() {
    if (!spawned_) return;
    std::call_once(teardown_, [this] {
        ring_->close();  // from here on output is drained and dropped
        stop_input();
        close_console();  // CTRL_CLOSE_EVENT to the console's clients
        const bool may_kill = !pty_detail::test_suppress_kill();
        if (!exit_->wait_for(config_.terminate_grace) && may_kill) {
            if (job_) TerminateJobObject(job_.h, 1);
            else TerminateProcess(process_.h, 1);
            WaitForSingleObject(process_.h, 5000);
        }
        // The rest of the tree: whatever the child started that is still in
        // the job (detached from the console or not) goes with the session.
        if (job_ && may_kill) TerminateJobObject(job_.h, 1);
        SetEvent(stop_event_.h);
        if (waiter_.joinable()) waiter_.join();
        if (!exit_->done()) {
            DWORD code = 0;
            if (GetExitCodeProcess(process_.h, &code) && code != STILL_ACTIVE) set_exited(int(code));
        }
        cancel_and_join(writer_, writer_done_);
        cancel_and_join(reader_, reader_done_);
        output_done_ = true;
        in_write_.reset();
        out_read_.reset();
        if (may_kill) job_.reset();  // (kill-on-close; kept open under the test seam)
    });
}

namespace pty_detail {
size_t orphans_pending() { return 0; }
} // namespace pty_detail

std::unique_ptr<IPtyProcess> create_pty_win() { return std::make_unique<PtyWin>(); }

} // namespace bropty

#endif // _WIN32
