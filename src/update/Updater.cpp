#include "Updater.h"

#include "update/Sha256.h"

#include <array>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {

constexpr const char* kReleaseUrl = "https://github.com/DefectiveNull/AtlasWriter/releases/latest/download";
constexpr const char* kFeedName = "atlaswriter-update.txt";
constexpr const char* kMarkerName = "installed.marker";

#if defined(SDL_PLATFORM_WINDOWS)
constexpr const char* kPlatform = "windows-x64";
constexpr const char* kArchiveName = "AtlasWriter-windows-x64.zip";
constexpr const char* kExeName = "AtlasWriter.exe";
#else
constexpr const char* kPlatform = "linux-x64";
constexpr const char* kArchiveName = "AtlasWriter-linux-x64.tar.gz";
constexpr const char* kExeName = "AtlasWriter";
#endif

fs::path toPath(const std::string& utf8)
{
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string toUtf8(const fs::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

fs::path installDir()
{
    const char* base = SDL_GetBasePath();
    return base ? toPath(base) : fs::path();
}

// Windows' own copies, by full path; elsewhere whatever is on PATH.
std::string systemTool(const char* name)
{
#if defined(SDL_PLATFORM_WINDOWS)
    const char* root = SDL_getenv("SystemRoot");
    return std::string(root ? root : "C:\\Windows") + "\\System32\\" + name + ".exe";
#else
    return name;
#endif
}

bool parseVersion(const std::string& text, std::array<int, 3>& out)
{
    int a = 0, b = 0, c = 0;
    char end = 0;
    if (SDL_sscanf(text.c_str(), "%d.%d.%d%c", &a, &b, &c, &end) != 3)
        return false;
    out = {a, b, c};
    return true;
}

bool isNewer(const std::string& candidate)
{
    std::array<int, 3> theirs{}, ours{};
    return parseVersion(candidate, theirs) && parseVersion(ATLASWRITER_VERSION, ours) && theirs > ours;
}

std::string fileHash(const fs::path& path)
{
    size_t size = 0;
    void* data = SDL_LoadFile(toUtf8(path).c_str(), &size);
    if (!data)
        return {};
    std::string hash = sha256::hex(data, size);
    SDL_free(data);
    return hash;
}

// The feed: the version on the first line, then "<platform> <sha256>" lines.
bool readFeed(const fs::path& path, std::string& version, std::string& hash)
{
    size_t size = 0;
    void* data = SDL_LoadFile(toUtf8(path).c_str(), &size);
    if (!data)
        return false;
    std::istringstream feed(std::string(static_cast<const char*>(data), size));
    SDL_free(data);
    feed >> version;
    std::string platform, value;
    while (feed >> platform >> value) {
        if (platform == kPlatform) {
            hash = value;
            return !version.empty();
        }
    }
    return false;
}

} // namespace

Updater::~Updater()
{
    stop();
}

void Updater::start(Uint32 readyEvent, std::string feedUrl)
{
    readyEvent_ = readyEvent;
    feedUrl_ = feedUrl.empty() ? kReleaseUrl : std::move(feedUrl);
    thread_ = SDL_CreateThread(threadMain, "updater", this);
    if (!thread_)
        SDL_Log("Update check not started: %s", SDL_GetError());
}

void Updater::stop()
{
    stopping_ = true;
    {
        std::lock_guard lock(processMutex_);
        if (process_)
            SDL_KillProcess(process_, true);
    }
    if (thread_) {
        SDL_WaitThread(thread_, nullptr);
        thread_ = nullptr;
    }
}

int SDLCALL Updater::threadMain(void* self)
{
    static_cast<Updater*>(self)->work();
    return 0;
}

void Updater::work()
{
    const fs::path dir = installDir();
    std::error_code ec;
    if (dir.empty() || !fs::exists(dir / kMarkerName, ec))
        return;

    // Whatever an earlier update or a crashed run left behind. The running
    // executable from the last update is free to delete by now.
    const fs::path update = dir / "update";
    if (fs::is_directory(update, ec)) {
        for (const fs::directory_entry& entry : fs::directory_iterator(update, ec)) {
            if (entry.path().filename() != "ready")
                fs::remove_all(entry.path(), ec);
        }
    }

    if (!fs::exists(update / "ready", ec) && !check(update)) {
        fs::remove(update, ec); // only goes if empty
        return;
    }
    ready_ = true;
    SDL_Event event{};
    event.type = readyEvent_;
    SDL_PushEvent(&event);
}

// Downloads and unpacks a newer version, if there is one.
bool Updater::check(const fs::path& update)
{
    std::error_code ec;
    const fs::path work = update / ("tmp-" + std::to_string(SDL_GetPerformanceCounter()));
    fs::create_directories(work, ec);
    if (ec)
        return false;
    struct Cleanup
    {
        const fs::path& path;
        ~Cleanup()
        {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } cleanup{work};

    const std::string curl = systemTool("curl");
    const fs::path feed = work / kFeedName;
    if (!run({curl, "--fail", "--silent", "--location", "--max-time", "30", "--output", toUtf8(feed),
              feedUrl_ + "/" + kFeedName}))
        return false;

    std::string version, hash;
    if (!readFeed(feed, version, hash) || !isNewer(version))
        return false;

    const fs::path archive = work / kArchiveName;
    if (!run({curl, "--fail", "--silent", "--location", "--max-time", "1800", "--output", toUtf8(archive),
              feedUrl_ + "/" + kArchiveName}))
        return false;
    if (fileHash(archive) != hash) {
        SDL_Log("Update %s rejected: its checksum doesn't match", version.c_str());
        return false;
    }

    const fs::path unpacked = work / "unpacked";
    fs::create_directories(unpacked, ec);
    if (ec || !run({systemTool("tar"), "-xf", toUtf8(archive), "-C", toUtf8(unpacked)}))
        return false;

    const fs::path app = unpacked / "AtlasWriter";
    if (!fs::exists(app / kExeName, ec)) {
        SDL_Log("Update %s rejected: the archive has no executable", version.c_str());
        return false;
    }
    fs::rename(app, update / "ready", ec); // atomic, so "ready" is always complete
    return !ec;
}

// Runs a tool to completion; false if it failed or the app is closing.
bool Updater::run(const std::vector<std::string>& args)
{
    std::vector<const char*> argv;
    for (const std::string& arg : args)
        argv.push_back(arg.c_str());
    argv.push_back(nullptr);

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
#if defined(SDL_PLATFORM_WINDOWS)
    // Otherwise a console window flashes up. Elsewhere this would detach the
    // process and lose its exit code.
    SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
#endif

    SDL_Process* process = nullptr;
    {
        std::lock_guard lock(processMutex_);
        if (stopping_) {
            SDL_DestroyProperties(props);
            return false;
        }
        process = SDL_CreateProcessWithProperties(props);
        process_ = process;
    }
    SDL_DestroyProperties(props);
    if (!process) {
        SDL_Log("Update check: could not run %s: %s", args[0].c_str(), SDL_GetError());
        return false;
    }

    int exitCode = -1;
    SDL_WaitProcess(process, true, &exitCode);
    {
        std::lock_guard lock(processMutex_);
        process_ = nullptr;
    }
    SDL_DestroyProcess(process);
    return exitCode == 0 && !stopping_;
}

bool applyStagedUpdate()
{
    const fs::path dir = installDir();
    std::error_code ec;
    if (dir.empty() || !fs::exists(dir / kMarkerName, ec))
        return false;

    // The rename claims the update, so with two windows open only one applies it.
    const fs::path update = dir / "update", staged = update / "applying", backup = update / "backup";
    fs::rename(update / "ready", staged, ec);
    if (ec)
        return false;
    fs::remove_all(backup, ec);
    fs::create_directories(backup, ec);

    std::vector<fs::path> names;
    for (const fs::directory_entry& entry : fs::directory_iterator(staged, ec)) {
        const fs::path name = entry.path().filename();
        if (name != "install.ps1" && name != "install.sh") // only used from the archive
            names.push_back(name);
    }

    // The running executable can be renamed even though it can't be replaced.
    size_t moved = 0;
    bool failed = static_cast<bool>(ec);
    for (; !failed && moved < names.size(); ++moved) {
        const fs::path& name = names[moved];
        if (fs::exists(dir / name, ec)) {
            fs::rename(dir / name, backup / name, ec);
            if (ec)
                break;
        }
        fs::rename(staged / name, dir / name, ec);
        if (ec)
            break;
    }
    failed = failed || moved < names.size();

    if (failed) {
        SDL_Log("Update could not be applied: %s", ec.message().c_str());
        for (size_t i = 0; i <= moved && i < names.size(); ++i) {
            std::error_code ignored;
            if (fs::exists(backup / names[i], ignored)) {
                fs::remove_all(dir / names[i], ignored);
                fs::rename(backup / names[i], dir / names[i], ignored);
            }
        }
    }

    // The old executable stays until the next launch clears it out.
    fs::remove_all(staged, ec);
    fs::remove_all(backup, ec);
    fs::remove(update, ec);
    return !failed;
}
