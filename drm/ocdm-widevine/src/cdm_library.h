// Loads the device's Widevine CE CDM at run time (dlopen), so libocdm has no link-time dependency on a
// vendor binary. Only the three static entry points need resolving; everything else goes through the vtable.
#pragma once

#include <widevine/cdm.h>

#include <string>

namespace ocdm_widevine {

class CdmLibrary {
public:
    // |path| defaults to $OCDM_WIDEVINE_CDM_PATH or the TV's /usr/lib copy.
    bool load(std::string path = std::string());
    bool loaded() const { return handle_ && initialize_ && create_ && version_; }
    const std::string& error() const { return error_; }

    widevine::Cdm::Status initialize(widevine::Cdm::SecureOutputType type, const widevine::Cdm::ClientInfo& info,
        widevine::Cdm::IStorage* storage, widevine::Cdm::IClock* clock, widevine::Cdm::ITimer* timer,
        widevine::Cdm::LogLevel level) const;
    widevine::Cdm* create(widevine::Cdm::IEventListener* listener, widevine::Cdm::IStorage* storage,
        bool privacy_mode) const;
    const char* version() const;

private:
    typedef widevine::Cdm::Status (*InitializeFn)(widevine::Cdm::SecureOutputType, const widevine::Cdm::ClientInfo&,
        widevine::Cdm::IStorage*, widevine::Cdm::IClock*, widevine::Cdm::ITimer*, widevine::Cdm::LogLevel);
    typedef widevine::Cdm* (*CreateFn)(widevine::Cdm::IEventListener*, widevine::Cdm::IStorage*, bool);
    typedef const char* (*VersionFn)();

    void* handle_ = nullptr;
    InitializeFn initialize_ = nullptr;
    CreateFn create_ = nullptr;
    VersionFn version_ = nullptr;
    std::string error_;
};

} // namespace ocdm_widevine
