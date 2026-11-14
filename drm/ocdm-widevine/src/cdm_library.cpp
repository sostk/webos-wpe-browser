#include "cdm_library.h"

#include <cstdlib>
#include <dlfcn.h>
#include <string>

namespace ocdm_widevine {

// Mangled names of widevine::Cdm::initialize (6-argument form), ::create (3-argument form) and ::version,
// as exported by libwidevine_ce_cdm_shared.so 16.3.0 (docs/widevine-cdm-abi.md).
static const char kInitialize[] = "_ZN8widevine3Cdm10initializeENS0_16SecureOutputTypeERKNS0_10ClientInfoEPNS0_8IStorageEPNS0_6IClockEPNS0_6ITimerENS0_8LogLevelE";
static const char kCreate[] = "_ZN8widevine3Cdm6createEPNS0_14IEventListenerEPNS0_8IStorageEb";
static const char kVersion[] = "_ZN8widevine3Cdm7versionEv";

// Major version of a CDM version string ("16.3.0", "fake-16.3.0"), or -1.
int cdm_major_version(const char* version)
{
    if (!version)
        return -1;
    while (*version && (*version < '0' || *version > '9'))
        ++version;
    if (!*version)
        return -1;
    return atoi(version);
}

bool CdmLibrary::load(std::string path)
{
    if (loaded())
        return true;
    if (path.empty()) {
        const char* env = getenv("OCDM_WIDEVINE_CDM_PATH");
        path = env && *env ? env : "/usr/lib/libwidevine_ce_cdm_shared.so";
    }
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
        const char* why = dlerror();
        error_ = why ? why : "dlopen failed";
        return false;
    }
    initialize_ = reinterpret_cast<InitializeFn>(dlsym(handle_, kInitialize));
    create_ = reinterpret_cast<CreateFn>(dlsym(handle_, kCreate));
    version_ = reinterpret_cast<VersionFn>(dlsym(handle_, kVersion));
    if (!loaded()) {
        error_ = path + ": missing widevine::Cdm entry points";
        dlclose(handle_);
        handle_ = nullptr;
        return false;
    }
    // cdm.h is the 16.x interface (vtable slots, the batch decrypt). 14.0 and 15.1 (webOS 4.5, 5) export the same
    // entry points but have another vtable, so calling into them would crash; 17.x renamed initialize anyway.
    // OCDM_WIDEVINE_ANY_VERSION=1 skips this check for experiments.
    const char* any = getenv("OCDM_WIDEVINE_ANY_VERSION");
    const int major = cdm_major_version(version_());
    if (major != kSupportedMajor && !(any && *any == '1')) {
        error_ = path + ": Widevine CDM " + version_() + " is not supported (only " + std::to_string(kSupportedMajor) +
            ".x)";
        initialize_ = nullptr;
        create_ = nullptr;
        version_ = nullptr;
        dlclose(handle_);
        handle_ = nullptr;
        return false;
    }
    return true;
}

widevine::Cdm::Status CdmLibrary::initialize(widevine::Cdm::SecureOutputType type, const widevine::Cdm::ClientInfo& info,
    widevine::Cdm::IStorage* storage, widevine::Cdm::IClock* clock, widevine::Cdm::ITimer* timer,
    widevine::Cdm::LogLevel level) const
{
    return initialize_(type, info, storage, clock, timer, level);
}

widevine::Cdm* CdmLibrary::create(widevine::Cdm::IEventListener* listener, widevine::Cdm::IStorage* storage,
    bool privacy_mode) const
{
    return create_(listener, storage, privacy_mode);
}

const char* CdmLibrary::version() const
{
    return version_();
}

} // namespace ocdm_widevine
