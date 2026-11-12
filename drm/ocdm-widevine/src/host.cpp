#include "host.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace ocdm_widevine {

namespace {

void make_dirs(const std::string& path)
{
    std::string partial;
    for (size_t i = 0; i < path.size(); ++i) {
        partial += path[i];
        if ((path[i] == '/' && i > 0) || i + 1 == path.size())
            mkdir(partial.c_str(), 0700);
    }
}

bool write_all(int fd, const char* data, size_t length)
{
    while (length) {
        ssize_t n = ::write(fd, data, length);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        data += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}

} // namespace

Host::Host(std::string directory)
    : directory_(std::move(directory))
{
    while (directory_.size() > 1 && directory_.back() == '/')
        directory_.pop_back();
    make_dirs(directory_);
    struct stat st;
    usable_ = stat(directory_.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && access(directory_.c_str(), R_OK | W_OK | X_OK) == 0;
    if (!usable_)
        fprintf(stderr, "ocdm-widevine: storage directory %s is not usable: %s\n", directory_.c_str(), strerror(errno));
    timer_thread_ = std::thread([this] { timer_loop(); });
}

Host::~Host()
{
    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        stopping_ = true;
        timers_.clear();
    }
    timer_cv_.notify_all();
    if (timer_thread_.joinable())
        timer_thread_.join();
}

// The CDM uses flat names such as "cert.bin", "usgtable.bin" and "ksid1234.lic".
// Anything with a path separator or a leading dot is refused so it cannot leave the directory.
std::string Host::path_for(const std::string& name) const
{
    if (name.empty() || name[0] == '.' || name.find('/') != std::string::npos || name.find('\0') != std::string::npos)
        return std::string();
    return directory_ + "/" + name;
}

bool Host::read(const std::string& name, std::string* data)
{
    std::string path = path_for(name);
    if (path.empty() || !data)
        return false;
    std::lock_guard<std::mutex> lock(storage_mutex_);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    std::string contents;
    char buffer[8192];
    size_t n;
    while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0)
        contents.append(buffer, n);
    bool ok = !ferror(f);
    fclose(f);
    if (ok)
        data->swap(contents);
    return ok;
}

// Written to a temporary file, synced, then renamed over the target: a crash or power cut leaves
// either the old file or the new one, never a torn device certificate or license.
bool Host::write(const std::string& name, const std::string& data)
{
    std::string path = path_for(name);
    if (path.empty())
        return false;
    std::lock_guard<std::mutex> lock(storage_mutex_);
    std::string temporary = path + ".tmp";
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return false;
    bool ok = write_all(fd, data.data(), data.size()) && fsync(fd) == 0;
    ok = (close(fd) == 0) && ok;
    if (ok && rename(temporary.c_str(), path.c_str()) == 0)
        return true;
    unlink(temporary.c_str());
    return false;
}

bool Host::exists(const std::string& name)
{
    std::string path = path_for(name);
    if (path.empty())
        return false;
    std::lock_guard<std::mutex> lock(storage_mutex_);
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// An empty name means "remove everything" (the CDM's delete-all path).
bool Host::remove(const std::string& name)
{
    if (name.empty()) {
        std::vector<std::string> names;
        list(&names);
        std::lock_guard<std::mutex> lock(storage_mutex_);
        for (const std::string& each : names)
            unlink((directory_ + "/" + each).c_str());
        return true;
    }
    std::string path = path_for(name);
    if (path.empty())
        return false;
    std::lock_guard<std::mutex> lock(storage_mutex_);
    return unlink(path.c_str()) == 0 || errno == ENOENT;
}

int32_t Host::size(const std::string& name)
{
    std::string path = path_for(name);
    if (path.empty())
        return -1;
    std::lock_guard<std::mutex> lock(storage_mutex_);
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    return st.st_size > INT32_MAX ? -1 : static_cast<int32_t>(st.st_size);
}

bool Host::list(std::vector<std::string>* names)
{
    if (!names)
        return false;
    names->clear();
    std::lock_guard<std::mutex> lock(storage_mutex_);
    DIR* dir = opendir(directory_.c_str());
    if (!dir)
        return false;
    while (struct dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name.empty() || name[0] == '.')
            continue;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".tmp") == 0)
            continue;
        struct stat st;
        if (stat((directory_ + "/" + name).c_str(), &st) == 0 && S_ISREG(st.st_mode))
            names->push_back(name);
    }
    closedir(dir);
    std::sort(names->begin(), names->end());
    return true;
}

int64_t Host::now()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

int64_t Host::monotonic_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void Host::setTimeout(int64_t delay_ms, IClient* client, void* context)
{
    if (!client)
        return;
    if (delay_ms < 0)
        delay_ms = 0;
    {
        std::lock_guard<std::mutex> lock(timer_mutex_);
        if (stopping_)
            return;
        timers_.push_back(Timer { monotonic_ms() + delay_ms, client, context });
    }
    timer_cv_.notify_all();
}

void Host::cancel(IClient* client)
{
    std::lock_guard<std::mutex> lock(timer_mutex_);
    timers_.erase(std::remove_if(timers_.begin(), timers_.end(), [client](const Timer& t) { return t.client == client; }),
        timers_.end());
}

// Fires due timers one at a time, outside the lock, so a callback may schedule or cancel timers.
void Host::timer_loop()
{
    std::unique_lock<std::mutex> lock(timer_mutex_);
    while (!stopping_) {
        if (timers_.empty()) {
            timer_cv_.wait(lock);
            continue;
        }
        auto next = std::min_element(timers_.begin(), timers_.end(),
            [](const Timer& a, const Timer& b) { return a.due_ms < b.due_ms; });
        int64_t wait = next->due_ms - monotonic_ms();
        if (wait > 0) {
            timer_cv_.wait_for(lock, std::chrono::milliseconds(wait));
            continue;
        }
        Timer due = *next;
        timers_.erase(next);
        lock.unlock();
        due.client->onTimerExpired(due.context);
        lock.lock();
    }
}

} // namespace ocdm_widevine
