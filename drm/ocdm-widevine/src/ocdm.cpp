// OpenCDM C API implemented in-process on the TV's Widevine CE CDM.
//
// One CDM instance serves the whole process (Cdm::initialize may run once). Every CDM call holds |cdm_lock|,
// which the Host's timer thread also takes, so the CDM is never entered from two threads at once.
// The CDM calls our listener synchronously from inside its own methods; the listener never calls back into
// the CDM itself, it hands that work (key status queries, the final close of a released session) to a
// dispatcher thread.
#include "cdm_library.h"
#include "host.h"
#include "http.h"
#include "ocdm_internal.h"

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using widevine::Cdm;

struct OpenCDMSystem {
    std::string key_system;
};

struct OpenCDMSession {
    std::atomic<int> refs { 1 };
    std::string id;                       // the CDM's session id; immutable once registered
    OpenCDMSessionCallbacks callbacks {};
    void* user_data = nullptr;
    bool open = true;                     // guarded by Context::cdm_lock
    std::atomic<bool> refresh_pending { false };
    std::map<std::string, KeyStatus> keys; // guarded by Context::sessions_mutex
};

namespace ocdm_widevine {

static const char kWidevine[] = "com.widevine.alpha";
// Google's public Widevine provisioning endpoint, as built into the TV's own CDM (libwvcdm_shared) and Chromium.
static const char kDefaultProvisioningUrl[] =
    "https://www.googleapis.com/certificateprovisioning/v1/devicecertificates/create?key=AIzaSyB-5OLKTx2iU5mko18DfdwK5611JIjbUhE";

static bool debug_enabled()
{
    static const bool enabled = getenv("OCDM_WIDEVINE_DEBUG") != nullptr;
    return enabled;
}

static void log(bool debug, const char* format, ...) __attribute__((format(printf, 2, 3)));
static void log(bool debug, const char* format, ...)
{
    if (debug && !debug_enabled())
        return;
    va_list args;
    va_start(args, format);
    fputs("ocdm-widevine: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
}

static std::string storage_directory()
{
    if (const char* dir = getenv("OCDM_WIDEVINE_STORAGE"))
        if (*dir)
            return dir;
    if (const char* data = getenv("XDG_DATA_HOME"))
        if (*data)
            return std::string(data) + "/widevine";
    if (const char* home = getenv("HOME"))
        if (*home)
            return std::string(home) + "/.local/share/widevine";
    return "/tmp/ocdm-widevine";
}

static KeyStatus to_ocdm(Cdm::KeyStatus status)
{
    switch (status) {
    case Cdm::kUsable: return Usable;
    case Cdm::kExpired: return Expired;
    case Cdm::kOutputRestricted: return OutputRestricted;
    case Cdm::kStatusPending: return StatusPending;
    case Cdm::kReleased: return Released;
    case Cdm::kInternalError: return InternalError;
    }
    return InternalError;
}

static bool printable(const std::string& s)
{
    for (unsigned char c : s)
        if (c < 0x20 || c > 0x7e)
            return false;
    return !s.empty();
}

static std::string base64url(const std::string& in)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    unsigned value = 0;
    int bits = -6;
    for (unsigned char c : in) {
        value = (value << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(table[(value >> bits) & 63]);
            bits -= 6;
        }
    }
    if (bits > -6)
        out.push_back(table[((value << 8) >> (bits + 8)) & 63]);
    return out;
}

class Context final : public Cdm::IEventListener {
public:
    // The process-wide context, created on first use; nullptr if the CDM cannot be used here.
    static Context* get();

    std::recursive_mutex cdm_lock;
    Cdm* cdm = nullptr;

    std::mutex sessions_mutex;
    std::condition_variable keys_changed;
    std::map<std::string, OpenCDMSession*> sessions; // open sessions by id (not owning)

    bool ensure_provisioned();   // call with cdm_lock held
    void schedule_refresh(OpenCDMSession*);
    void post(std::function<void()>);
    void release(OpenCDMSession*);

    void register_session(OpenCDMSession*);
    void unregister_session(OpenCDMSession*);
    OpenCDMSession* acquire(const std::string& id); // +1 ref, or nullptr

    // Cdm::IEventListener (called by the CDM, with cdm_lock held)
    void onMessage(const std::string& session_id, Cdm::MessageType type, const std::string& message) override;
    void onKeyStatusesChange(const std::string& session_id, bool has_new_usable_key) override;
    void onRemoveComplete(const std::string& session_id) override;
    void onDeferredComplete(const std::string& session_id, Cdm::Status result) override;
    void onDirectIndividualizationRequest(const std::string& session_id, const std::string& request) override;

private:
    Context() = default;
    bool start();
    void refresh(OpenCDMSession*);
    void run_dispatcher();
    void destroy(OpenCDMSession*);

    CdmLibrary library_;
    std::unique_ptr<Host> host_;
    std::thread dispatcher_;
    std::mutex tasks_mutex_;
    std::condition_variable tasks_ready_;
    std::deque<std::function<void()>> tasks_;
};

Context* Context::get()
{
    static Context* instance = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        Context* context = new Context;
        if (context->start())
            instance = context;
        // On failure the half-built context is leaked on purpose: the CDM may hold pointers into it.
    });
    return instance;
}

bool Context::start()
{
    if (!library_.load()) {
        log(false, "Widevine CDM not available: %s", library_.error().c_str());
        return false;
    }
    host_.reset(new Host(storage_directory()));
    if (!host_->usable())
        return false;
    host_->set_cdm_lock(&cdm_lock);
    dispatcher_ = std::thread([this] { run_dispatcher(); });

    Cdm::ClientInfo info;
    info.product_name = "WPE WebKit";
    info.company_name = "LG Electronics";
    info.device_name = "Linux";
    info.model_name = "webOS TV";
    info.arch_name = "armv7l";
    info.build_info = "ocdm-widevine";

    std::lock_guard<std::recursive_mutex> lock(cdm_lock);
    Cdm::Status status = library_.initialize(Cdm::kNoSecureOutput, info, host_.get(), host_.get(), host_.get(), Cdm::kErrors);
    if (status != Cdm::kSuccess) {
        log(false, "Widevine CDM %s: initialize failed (%d)", library_.version(), status);
        return false;
    }
    cdm = library_.create(this, host_.get(), false);
    if (!cdm) {
        log(false, "Widevine CDM %s: create failed", library_.version());
        return false;
    }
    log(true, "Widevine CDM %s ready, storage %s", library_.version(), host_->directory().c_str());
    return true;
}

// Fetches this device's Widevine certificate once; it is kept in the storage directory afterwards.
bool Context::ensure_provisioned()
{
    if (cdm->isProvisioned())
        return true;
    std::string request;
    Cdm::Status status = cdm->getProvisioningRequest(&request);
    if (status != Cdm::kSuccess || request.empty()) {
        log(false, "provisioning: no request (%d)", status);
        return false;
    }
    const char* env = getenv("OCDM_WIDEVINE_PROVISIONING_URL");
    std::string url = env && *env ? env : kDefaultProvisioningUrl;
    url += url.find('?') == std::string::npos ? "?" : "&";
    url += "signedRequest=" + (printable(request) ? request : base64url(request));
    log(true, "provisioning this device");
    HttpResult reply = http_post(url, std::string(), "application/json", 20);
    if (!reply.ok) {
        log(false, "provisioning: %s", reply.error.c_str());
        return false;
    }
    status = cdm->handleProvisioningResponse(reply.body);
    if (status != Cdm::kSuccess || !cdm->isProvisioned()) {
        log(false, "provisioning: reply refused (%d)", status);
        return false;
    }
    log(true, "provisioned");
    return true;
}

void Context::register_session(OpenCDMSession* session)
{
    std::lock_guard<std::mutex> lock(sessions_mutex);
    sessions[session->id] = session;
}

void Context::unregister_session(OpenCDMSession* session)
{
    std::lock_guard<std::mutex> lock(sessions_mutex);
    auto it = sessions.find(session->id);
    if (it != sessions.end() && it->second == session)
        sessions.erase(it);
    keys_changed.notify_all();
}

OpenCDMSession* Context::acquire(const std::string& id)
{
    std::lock_guard<std::mutex> lock(sessions_mutex);
    auto it = sessions.find(id);
    if (it == sessions.end())
        return nullptr;
    it->second->refs.fetch_add(1);
    return it->second;
}

void Context::post(std::function<void()> task)
{
    {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        tasks_.push_back(std::move(task));
    }
    tasks_ready_.notify_one();
}

void Context::run_dispatcher()
{
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(tasks_mutex_);
            tasks_ready_.wait(lock, [this] { return !tasks_.empty(); });
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task();
    }
}

// Drops a reference. The last one closes the CDM session; that runs on the dispatcher so it never happens
// inside a CDM callback.
void Context::release(OpenCDMSession* session)
{
    if (session->refs.fetch_sub(1) == 1)
        post([this, session] { destroy(session); });
}

void Context::destroy(OpenCDMSession* session)
{
    {
        std::lock_guard<std::recursive_mutex> lock(cdm_lock);
        if (session->open) {
            cdm->close(session->id);
            session->open = false;
        }
    }
    unregister_session(session);
    delete session;
}

void Context::schedule_refresh(OpenCDMSession* session)
{
    if (session->refresh_pending.exchange(true))
        return;
    session->refs.fetch_add(1);
    post([this, session] {
        session->refresh_pending = false;
        refresh(session);
        release(session);
    });
}

// Reads the session's key statuses from the CDM and reports them: one key_update per key, then keys_updated.
void Context::refresh(OpenCDMSession* session)
{
    Cdm::KeyStatusMap statuses;
    {
        std::lock_guard<std::recursive_mutex> lock(cdm_lock);
        if (!session->open)
            return;
        Cdm::Status status = cdm->getKeyStatuses(session->id, &statuses);
        if (status != Cdm::kSuccess) {
            log(false, "session %s: getKeyStatuses failed (%d)", session->id.c_str(), status);
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(sessions_mutex);
        session->keys.clear();
        for (const auto& entry : statuses)
            session->keys[entry.first] = to_ocdm(entry.second);
    }
    keys_changed.notify_all();
    log(true, "session %s: %zu key(s)", session->id.c_str(), statuses.size());
    for (const auto& entry : statuses)
        if (session->callbacks.key_update_callback && entry.first.size() <= 255)
            session->callbacks.key_update_callback(session, session->user_data,
                reinterpret_cast<const uint8_t*>(entry.first.data()), static_cast<uint8_t>(entry.first.size()));
    if (session->callbacks.keys_updated_callback)
        session->callbacks.keys_updated_callback(session, session->user_data);
}

void Context::onMessage(const std::string& session_id, Cdm::MessageType type, const std::string& message)
{
    OpenCDMSession* session = acquire(session_id);
    if (!session) {
        log(false, "message for unknown session %s", session_id.c_str());
        return;
    }
    // WebKit reads the EME message type from a "<n>:Type:" prefix (0 request, 1 renewal, 2 release, 3 individualization).
    int eme_type = (type >= Cdm::kLicenseRequest && type <= Cdm::kIndividualizationRequest) ? type : Cdm::kLicenseRequest;
    std::string payload = std::to_string(eme_type) + ":Type:" + message;
    log(true, "session %s: message type %d, %zu bytes", session_id.c_str(), eme_type, message.size());
    if (payload.size() > 0xffff) {
        if (session->callbacks.error_message_callback)
            session->callbacks.error_message_callback(session, session->user_data, "license message too large");
    } else if (session->callbacks.process_challenge_callback)
        session->callbacks.process_challenge_callback(session, session->user_data, "",
            reinterpret_cast<const uint8_t*>(payload.data()), static_cast<uint16_t>(payload.size()));
    release(session);
}

void Context::onKeyStatusesChange(const std::string& session_id, bool)
{
    if (OpenCDMSession* session = acquire(session_id)) {
        schedule_refresh(session);
        release(session);
    }
}

void Context::onRemoveComplete(const std::string& session_id)
{
    log(true, "session %s: remove complete", session_id.c_str());
}

void Context::onDeferredComplete(const std::string& session_id, Cdm::Status result)
{
    log(true, "session %s: deferred operation done (%d)", session_id.c_str(), result);
}

void Context::onDirectIndividualizationRequest(const std::string& session_id, const std::string&)
{
    log(false, "session %s: direct individualization requested (not supported)", session_id.c_str());
}

OpenCDMError decrypt_in_place(OpenCDMSession* session, const SampleDescription& sample, uint8_t* data, size_t length)
{
    Context* context = Context::get();
    if (!context || !session)
        return ERROR_INVALID_SESSION;
    if (!data || !length)
        return ERROR_NONE;
    if (!sample.key_id || !sample.key_id_length || !sample.iv || !sample.iv_length || sample.iv_length > 16)
        return ERROR_INVALID_DECRYPT_BUFFER;

    std::vector<Cdm::Subsample> subsamples = sample.subsamples;
    if (subsamples.empty())
        subsamples.push_back(Cdm::Subsample { 0, static_cast<uint32_t>(length) });
    uint64_t covered = 0;
    for (const Cdm::Subsample& s : subsamples)
        covered += static_cast<uint64_t>(s.clear_bytes) + s.protected_bytes;
    if (covered != length)
        return ERROR_INVALID_DECRYPT_BUFFER;

    uint8_t iv[16] = { 0 }; // 8-byte CENC IVs are the high half of the counter block
    memcpy(iv, sample.iv, sample.iv_length);
    // Decrypting into a separate buffer: in-place output has not been verified with this CDM.
    std::vector<uint8_t> output(length);

    Cdm::Sample one;
    one.iv = iv;
    one.iv_length = 16;
    one.data = data;
    one.data_length = static_cast<uint32_t>(length);
    one.subsamples = subsamples.data();
    one.subsamples_length = static_cast<uint32_t>(subsamples.size());
    one.output = output.data();
    one.output_offset = 0;
    one.output_length = static_cast<uint32_t>(length);

    Cdm::DecryptionBatch batch;
    batch.samples = &one;
    batch.samples_length = 1;
    batch.key_id = sample.key_id;
    batch.key_id_length = static_cast<uint32_t>(sample.key_id_length);
    batch.pattern_encrypted = sample.pattern_encrypted;
    batch.pattern_clear = sample.pattern_clear;
    batch.is_secure = false;
    batch.encryption_scheme = sample.scheme;
    batch.is_video = sample.is_video;

    Cdm::Status status;
    {
        std::lock_guard<std::recursive_mutex> lock(context->cdm_lock);
        if (!session->open)
            return ERROR_INVALID_SESSION;
        status = context->cdm->decrypt(session->id, batch);
    }
    if (status != Cdm::kSuccess) {
        log(false, "session %s: decrypt of %zu bytes failed (%d)", session->id.c_str(), length, status);
        return status == Cdm::kNoKey ? ERROR_INVALID_SESSION : ERROR_FAIL;
    }
    memcpy(data, output.data(), length);
    return ERROR_NONE;
}

} // namespace ocdm_widevine

using namespace ocdm_widevine;

extern "C" {

OpenCDMError opencdm_is_type_supported(const char keySystem[], const char mimeType[])
{
    if (!keySystem || strcmp(keySystem, kWidevine))
        return ERROR_KEYSYSTEM_NOT_SUPPORTED;
    static CdmLibrary probe;
    static const bool available = probe.load();
    if (!available)
        return ERROR_KEYSYSTEM_NOT_SUPPORTED;
    if (!mimeType || !*mimeType)
        return ERROR_NONE;
    std::string container(mimeType);
    container = container.substr(0, container.find(';'));
    while (!container.empty() && container.back() == ' ')
        container.pop_back();
    for (const char* supported : { "video/mp4", "audio/mp4", "video/webm", "audio/webm" })
        if (container == supported)
            return ERROR_NONE;
    return ERROR_INVALID_ARG;
}

struct OpenCDMSystem* opencdm_create_system(const char keySystem[])
{
    if (!keySystem || strcmp(keySystem, kWidevine) || !Context::get())
        return nullptr;
    OpenCDMSystem* system = new OpenCDMSystem;
    system->key_system = keySystem;
    return system;
}

OpenCDMError opencdm_destruct_system(struct OpenCDMSystem* system)
{
    delete system;
    return ERROR_NONE;
}

// Service certificates (privacy mode) are not wired yet: setServiceCertificate's slot is unverified.
// Reporting "unsupported" makes MediaKeys.setServerCertificate() resolve to false, as EME specifies.
bool opencdm_system_supports_server_certificate(struct OpenCDMSystem*)
{
    return false;
}

OpenCDMError opencdm_system_set_server_certificate(struct OpenCDMSystem*, const uint8_t[], const uint16_t)
{
    return ERROR_INTERFACE_NOT_IMPLEMENTED;
}

struct OpenCDMSession* opencdm_get_system_session(struct OpenCDMSystem*, const uint8_t keyId[], const uint8_t length,
    const uint32_t waitTime)
{
    Context* context = Context::get();
    if (!context || !keyId || !length)
        return nullptr;
    std::string key(reinterpret_cast<const char*>(keyId), length);
    std::unique_lock<std::mutex> lock(context->sessions_mutex);
    OpenCDMSession* found = nullptr;
    context->keys_changed.wait_for(lock, std::chrono::milliseconds(waitTime), [&] {
        for (const auto& entry : context->sessions) {
            auto it = entry.second->keys.find(key);
            if (it == entry.second->keys.end())
                continue;
            if (!found || it->second == Usable)
                found = entry.second;
            if (it->second == Usable)
                break;
        }
        return found != nullptr;
    });
    if (found)
        found->refs.fetch_add(1);
    return found;
}

OpenCDMError opencdm_construct_session(struct OpenCDMSystem* system, const LicenseType licenseType,
    const char initDataType[], const uint8_t initData[], const uint16_t initDataLength, const uint8_t[],
    const uint16_t, OpenCDMSessionCallbacks* callbacks, void* userData, struct OpenCDMSession** session)
{
    if (session)
        *session = nullptr;
    Context* context = Context::get();
    if (!context || !system || !session)
        return ERROR_INVALID_ARG;
    // Persistent sessions need load/remove, whose slots are not verified yet.
    if (licenseType != Temporary)
        return ERROR_INTERFACE_NOT_IMPLEMENTED;

    Cdm::InitDataType type;
    std::string kind = initDataType ? initDataType : "";
    if (kind == "cenc")
        type = Cdm::kCenc;
    else if (kind == "webm")
        type = Cdm::kWebM;
    else if (kind == "keyids")
        type = Cdm::kKeyIds;
    else
        return ERROR_INVALID_ARG;

    std::lock_guard<std::recursive_mutex> lock(context->cdm_lock);
    if (!context->ensure_provisioned())
        return ERROR_FAIL;
    std::string id;
    Cdm::Status status = context->cdm->createSession(Cdm::kTemporary, &id);
    if (status == Cdm::kNeedsDeviceCertificate && context->ensure_provisioned())
        status = context->cdm->createSession(Cdm::kTemporary, &id);
    if (status != Cdm::kSuccess || id.empty()) {
        log(false, "createSession failed (%d)", status);
        return ERROR_FAIL;
    }

    OpenCDMSession* created = new OpenCDMSession;
    created->id = id;
    if (callbacks)
        created->callbacks = *callbacks;
    created->user_data = userData;
    context->register_session(created);

    // The license request arrives synchronously through onMessage -> process_challenge_callback.
    std::string data(reinterpret_cast<const char*>(initData), initData ? initDataLength : 0);
    status = context->cdm->generateRequest(id, type, data);
    if (status != Cdm::kSuccess) {
        log(false, "session %s: generateRequest failed (%d)", id.c_str(), status);
        context->cdm->close(id);
        created->open = false;
        context->unregister_session(created);
        delete created;
        return ERROR_FAIL;
    }
    *session = created;
    return ERROR_NONE;
}

OpenCDMError opencdm_destruct_session(struct OpenCDMSession* session)
{
    Context* context = Context::get();
    if (!context || !session)
        return ERROR_INVALID_SESSION;
    context->release(session);
    return ERROR_NONE;
}

const char* opencdm_session_id(const struct OpenCDMSession* session)
{
    return session ? session->id.c_str() : "";
}

KeyStatus opencdm_session_status(const struct OpenCDMSession* session, const uint8_t keyId[], const uint8_t length)
{
    Context* context = Context::get();
    if (!context || !session || !keyId)
        return InternalError;
    std::lock_guard<std::mutex> lock(context->sessions_mutex);
    auto it = session->keys.find(std::string(reinterpret_cast<const char*>(keyId), length));
    return it == session->keys.end() ? StatusPending : it->second;
}

OpenCDMError opencdm_session_load(struct OpenCDMSession*)
{
    return ERROR_INTERFACE_NOT_IMPLEMENTED;
}

OpenCDMError opencdm_session_update(struct OpenCDMSession* session, const uint8_t keyMessage[], const uint16_t keyLength)
{
    Context* context = Context::get();
    if (!context || !session)
        return ERROR_INVALID_SESSION;
    std::string response(reinterpret_cast<const char*>(keyMessage), keyMessage ? keyLength : 0);
    Cdm::Status status;
    {
        std::lock_guard<std::recursive_mutex> lock(context->cdm_lock);
        if (!session->open)
            return ERROR_INVALID_SESSION;
        status = context->cdm->update(session->id, response);
    }
    if (status != Cdm::kSuccess) {
        log(false, "session %s: update failed (%d)", session->id.c_str(), status);
        return ERROR_FAIL;
    }
    // WebKit completes the update when keys_updated_callback arrives.
    context->schedule_refresh(session);
    return ERROR_NONE;
}

OpenCDMError opencdm_session_remove(struct OpenCDMSession*)
{
    return ERROR_INTERFACE_NOT_IMPLEMENTED;
}

OpenCDMError opencdm_session_close(struct OpenCDMSession* session)
{
    Context* context = Context::get();
    if (!context || !session)
        return ERROR_INVALID_SESSION;
    {
        std::lock_guard<std::recursive_mutex> lock(context->cdm_lock);
        if (session->open) {
            context->cdm->close(session->id);
            session->open = false;
        }
    }
    context->unregister_session(session);
    return ERROR_NONE;
}

} // extern "C"
