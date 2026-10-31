#include "Pty.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#endif

#include <algorithm>
#include <vector>

// ---------------------------------------------------------------------------
// Windows: ConPTY
// ---------------------------------------------------------------------------
#if defined(_WIN32)

namespace {

// Loaded at runtime so the app still starts on Windows versions without
// ConPTY (the terminal just reports that it can't start).
using PseudoConsole = void*;
using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, PseudoConsole*);
using ResizePseudoConsoleFn = HRESULT(WINAPI*)(PseudoConsole, COORD);
using ClosePseudoConsoleFn = void(WINAPI*)(PseudoConsole);

struct ConPtyApi
{
    CreatePseudoConsoleFn create = nullptr;
    ResizePseudoConsoleFn resize = nullptr;
    ClosePseudoConsoleFn close = nullptr;

    ConPtyApi()
    {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (!kernel)
            return;
        create = reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(kernel, "CreatePseudoConsole"));
        resize = reinterpret_cast<ResizePseudoConsoleFn>(GetProcAddress(kernel, "ResizePseudoConsole"));
        close = reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(kernel, "ClosePseudoConsole"));
    }
    bool ok() const { return create && resize && close; }
};

const ConPtyApi& api()
{
    static const ConPtyApi instance;
    return instance;
}

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

COORD coord(int cols, int rows)
{
    return {static_cast<SHORT>(std::max(1, cols)), static_cast<SHORT>(std::max(1, rows))};
}

} // namespace

struct Pty::Platform
{
    std::atomic<PseudoConsole> console{nullptr}; // cleared by the waiter when the shell exits
    HANDLE input = INVALID_HANDLE_VALUE;  // we write keystrokes here
    HANDLE output = INVALID_HANDLE_VALUE; // and read the shell's output here
    PROCESS_INFORMATION process{};
    std::thread waiter; // closes the console when the shell exits, ending readLoop
};

bool Pty::start(const std::string& shellCommand, const std::string& directory, int cols, int rows)
{
    stop();
    if (!api().ok()) {
        SDL_Log("Terminal: ConPTY is not available on this Windows version");
        return false;
    }

    auto p = std::make_unique<Platform>();
    HANDLE ptyInput = INVALID_HANDLE_VALUE, ptyOutput = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&ptyInput, &p->input, nullptr, 0) || !CreatePipe(&p->output, &ptyOutput, nullptr, 0)) {
        SDL_Log("Terminal: CreatePipe failed (%lu)", GetLastError());
        return false;
    }
    PseudoConsole console = nullptr;
    const HRESULT hr = api().create(coord(cols, rows), ptyInput, ptyOutput, 0, &console);
    p->console = console;
    CloseHandle(ptyInput);  // the console holds its own references now
    CloseHandle(ptyOutput);
    if (FAILED(hr)) {
        SDL_Log("Terminal: CreatePseudoConsole failed (0x%08lx)", static_cast<unsigned long>(hr));
        CloseHandle(p->input);
        CloseHandle(p->output);
        return false;
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    // Explicitly null std handles: otherwise a parent whose stdout is
    // redirected (or a console-subsystem build) passes its own handles down
    // and they take precedence over the pseudo console.
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrs(attrSize);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console,
                              sizeof(console), nullptr, nullptr);

    const std::wstring cwd = widen(directory);
    // The configured shell, else PowerShell (ships with every supported
    // Windows), else cmd.exe.
    std::vector<std::pair<std::wstring, std::string>> candidates; // command line, display name
    if (!shellCommand.empty()) {
        const std::string program = shellCommand.substr(0, shellCommand.find(' '));
        const size_t slash = program.find_last_of("/\\");
        candidates.emplace_back(widen(shellCommand), slash == std::string::npos ? program : program.substr(slash + 1));
    }
    candidates.emplace_back(L"powershell.exe -NoLogo", "powershell");
    const char* comspec = SDL_getenv("COMSPEC");
    candidates.emplace_back(comspec ? widen(comspec) : L"cmd.exe", "cmd");
    bool created = false;
    for (auto& [command, name] : candidates) {
        created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT,
                                 nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si.StartupInfo, &p->process);
        if (created) {
            shellName_ = name;
            break;
        }
        SDL_Log("Terminal: could not start '%s' (%lu)", name.c_str(), GetLastError());
    }
    DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!created) {
        SDL_Log("Terminal: CreateProcess failed (%lu)", GetLastError());
        api().close(console);
        CloseHandle(p->input);
        CloseHandle(p->output);
        return false;
    }

    platform_ = std::move(p);
    finished_ = false;
    started_ = true;
    reader_ = std::thread([this] { readLoop(); });
    // ConPTY keeps the output pipe open after the shell exits; closing the
    // console is what ends the reader.
    Platform* platform = platform_.get();
    platform_->waiter = std::thread([platform] {
        WaitForSingleObject(platform->process.hProcess, INFINITE);
        api().close(platform->console.exchange(nullptr));
    });
    return true;
}

void Pty::stop()
{
    if (!started_)
        return;
    started_ = false;
    TerminateProcess(platform_->process.hProcess, 0); // no-op if it already exited
    if (platform_->waiter.joinable())
        platform_->waiter.join(); // closes the console, which ends the reader
    if (reader_.joinable())
        reader_.join();
    CloseHandle(platform_->process.hProcess);
    CloseHandle(platform_->process.hThread);
    CloseHandle(platform_->input);
    CloseHandle(platform_->output);
    platform_.reset();
}

void Pty::write(std::string_view bytes)
{
    if (!started_ || finished_)
        return;
    while (!bytes.empty()) {
        DWORD written = 0;
        if (!WriteFile(platform_->input, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr))
            return;
        bytes.remove_prefix(written);
    }
}

void Pty::resize(int cols, int rows)
{
    if (!started_ || finished_)
        return;
    if (PseudoConsole console = platform_->console.load())
        api().resize(console, coord(cols, rows));
}

void Pty::readLoop()
{
    char buffer[16384];
    DWORD n = 0;
    while (ReadFile(platform_->output, buffer, sizeof(buffer), &n, nullptr) && n > 0) {
        {
            std::lock_guard lock(mutex_);
            pending_.append(buffer, n);
        }
        notify();
    }
    finished_ = true;
    eventQueued_ = false;
    notify();
}

// ---------------------------------------------------------------------------
// POSIX: forkpty
// ---------------------------------------------------------------------------
#else

struct Pty::Platform
{
    int master = -1;
    pid_t pid = -1;
    std::atomic<bool> quit{false};
};

bool Pty::start(const std::string& shellCommand, const std::string& directory, int cols, int rows)
{
    stop();
    auto p = std::make_unique<Platform>();
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(std::max(1, cols));
    ws.ws_row = static_cast<unsigned short>(std::max(1, rows));

    // A configured command line runs through /bin/sh -c, so it may have arguments.
    const char* shell = SDL_getenv("SHELL");
    if (!shell || !*shell)
        shell = "/bin/sh";
    const std::string program = shellCommand.empty() ? shell : shellCommand.substr(0, shellCommand.find(' '));
    const size_t slash = program.rfind('/');
    shellName_ = slash == std::string::npos ? program : program.substr(slash + 1);

    p->pid = forkpty(&p->master, nullptr, nullptr, &ws);
    if (p->pid < 0) {
        SDL_Log("Terminal: forkpty failed");
        return false;
    }
    if (p->pid == 0) { // child
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);
        if (!directory.empty() && chdir(directory.c_str()) != 0) {
            // stay in the inherited directory
        }
        if (shellCommand.empty())
            execl(shell, shell, static_cast<char*>(nullptr));
        else
            execl("/bin/sh", "sh", "-c", shellCommand.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    platform_ = std::move(p);
    finished_ = false;
    started_ = true;
    reader_ = std::thread([this] { readLoop(); });
    return true;
}

void Pty::stop()
{
    if (!started_)
        return;
    started_ = false;
    platform_->quit = true;
    kill(platform_->pid, SIGHUP);
    if (reader_.joinable())
        reader_.join();
    close(platform_->master);
    waitpid(platform_->pid, nullptr, 0);
    platform_.reset();
}

void Pty::write(std::string_view bytes)
{
    if (!started_ || finished_)
        return;
    while (!bytes.empty()) {
        const ssize_t n = ::write(platform_->master, bytes.data(), bytes.size());
        if (n <= 0)
            return;
        bytes.remove_prefix(static_cast<size_t>(n));
    }
}

void Pty::resize(int cols, int rows)
{
    if (!started_ || finished_)
        return;
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(std::max(1, cols));
    ws.ws_row = static_cast<unsigned short>(std::max(1, rows));
    ioctl(platform_->master, TIOCSWINSZ, &ws);
}

void Pty::readLoop()
{
    char buffer[16384];
    while (!platform_->quit) {
        pollfd fd{platform_->master, POLLIN, 0};
        const int ready = poll(&fd, 1, 100); // wake now and then to notice stop()
        if (ready < 0)
            break;
        if (ready == 0)
            continue;
        const ssize_t n = read(platform_->master, buffer, sizeof(buffer));
        if (n <= 0)
            break; // EIO: the shell exited
        {
            std::lock_guard lock(mutex_);
            pending_.append(buffer, static_cast<size_t>(n));
        }
        notify();
    }
    finished_ = true;
    eventQueued_ = false;
    notify();
}

#endif

// ---------------------------------------------------------------------------
// Shared
// ---------------------------------------------------------------------------

Pty::Pty(Uint32 eventType)
    : eventType_(eventType)
{
}

Pty::~Pty()
{
    stop();
}

void Pty::notify()
{
    if (eventQueued_.exchange(true))
        return; // the UI hasn't drained the last batch yet; it will get this too
    SDL_Event event{};
    event.type = eventType_;
    SDL_PushEvent(&event);
}

std::string Pty::takeOutput()
{
    eventQueued_ = false; // output arriving from now on needs a new event
    std::lock_guard lock(mutex_);
    std::string out;
    out.swap(pending_);
    return out;
}

bool Pty::exited()
{
    if (!started_ || !finished_)
        return false;
    std::lock_guard lock(mutex_);
    return pending_.empty();
}
