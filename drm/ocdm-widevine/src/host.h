// Platform services the Widevine CDM needs from us: file storage, a wall clock and one-shot timers.
#pragma once

#include <widevine/cdm.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ocdm_widevine {

class Host final : public widevine::Cdm::IStorage, public widevine::Cdm::IClock, public widevine::Cdm::ITimer {
public:
    // |directory| is created (0700) if missing. All CDM files live directly inside it.
    explicit Host(std::string directory);
    ~Host() override;

    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    const std::string& directory() const { return directory_; }
    // False when the directory could not be created or is not writable: the CDM would then fail every
    // provisioning and license call with no clear cause, so callers check this first.
    bool usable() const { return usable_; }

    // IStorage
    bool read(const std::string& name, std::string* data) override;
    bool write(const std::string& name, const std::string& data) override;
    bool exists(const std::string& name) override;
    bool remove(const std::string& name) override;
    int32_t size(const std::string& name) override;
    bool list(std::vector<std::string>* names) override;

    // IClock: real wall-clock milliseconds (the device certificate is refused if the clock is behind it).
    int64_t now() override;

    // ITimer: |client->onTimerExpired(context)| runs on the timer thread after |delay_ms|.
    void setTimeout(int64_t delay_ms, IClient* client, void* context) override;
    void cancel(IClient* client) override;

private:
    struct Timer {
        int64_t due_ms;
        IClient* client;
        void* context;
    };

    // Maps a CDM storage name to a path inside |directory_|; empty for names that could escape it.
    std::string path_for(const std::string& name) const;
    static int64_t monotonic_ms();
    void timer_loop();

    std::string directory_;
    bool usable_ = false;
    std::mutex storage_mutex_;

    std::mutex timer_mutex_;
    std::condition_variable timer_cv_;
    std::vector<Timer> timers_;
    bool stopping_ = false;
    std::thread timer_thread_;
};

} // namespace ocdm_widevine
