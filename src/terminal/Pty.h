#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

// A shell running in a pseudo terminal: ConPTY on Windows (10 1809+),
// forkpty() elsewhere.
//
// Output is read on a background thread and buffered; the first chunk after
// the buffer was last drained pushes `eventType`, so a burst of output wakes
// the UI once rather than once per read. The main thread then calls
// takeOutput().
class Pty
{
public:
    explicit Pty(Uint32 eventType);
    ~Pty();

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    // Starts `shell` (a command line; empty: the platform default) in
    // `directory` (empty: the current directory).
    bool start(const std::string& shell, const std::string& directory, int cols, int rows);
    void stop();

    void write(std::string_view bytes);
    void resize(int cols, int rows);

    // Main thread: output received since the last call.
    std::string takeOutput();
    // True once the shell has exited (after its last output was taken).
    bool exited();
    bool running() const { return started_; }
    const std::string& shellName() const { return shellName_; }

private:
    struct Platform; // OS handles
    void readLoop();
    void notify();

    Uint32 eventType_;
    std::unique_ptr<Platform> platform_;
    std::thread reader_;
    std::mutex mutex_;
    std::string pending_;
    std::atomic<bool> eventQueued_{false};
    std::atomic<bool> finished_{false};
    bool started_ = false;
    std::string shellName_;
};
