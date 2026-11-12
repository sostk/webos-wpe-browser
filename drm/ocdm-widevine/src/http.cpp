#include "http.h"

#include <cstdlib>
#include <dlfcn.h>
#include <mutex>

namespace ocdm_widevine {

namespace {

// The few libcurl symbols and option numbers we use (stable since libcurl 7.x; see curl.h).
typedef void CURL;
struct curl_slist;
enum {
    kOptWriteData = 10001,
    kOptUrl = 10002,
    kOptPostFields = 10015,
    kOptHttpHeader = 10023,
    kOptCaInfo = 10065,
    kOptTimeout = 13,
    kOptPost = 47,
    kOptFollowLocation = 52,
    kOptPostFieldSize = 60,
    kOptNoSignal = 99,
    kOptWriteFunction = 20011,
    kInfoResponseCode = 0x200002,
};

struct Curl {
    void* handle = nullptr;
    CURL* (*easy_init)() = nullptr;
    int (*easy_setopt)(CURL*, int, ...) = nullptr;
    int (*easy_perform)(CURL*) = nullptr;
    int (*easy_getinfo)(CURL*, int, ...) = nullptr;
    void (*easy_cleanup)(CURL*) = nullptr;
    const char* (*easy_strerror)(int) = nullptr;
    curl_slist* (*slist_append)(curl_slist*, const char*) = nullptr;
    void (*slist_free_all)(curl_slist*) = nullptr;

    bool load()
    {
        for (const char* name : { "libcurl.so.4", "libcurl.so" }) {
            handle = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (handle)
                break;
        }
        if (!handle)
            return false;
        easy_init = reinterpret_cast<decltype(easy_init)>(dlsym(handle, "curl_easy_init"));
        easy_setopt = reinterpret_cast<decltype(easy_setopt)>(dlsym(handle, "curl_easy_setopt"));
        easy_perform = reinterpret_cast<decltype(easy_perform)>(dlsym(handle, "curl_easy_perform"));
        easy_getinfo = reinterpret_cast<decltype(easy_getinfo)>(dlsym(handle, "curl_easy_getinfo"));
        easy_cleanup = reinterpret_cast<decltype(easy_cleanup)>(dlsym(handle, "curl_easy_cleanup"));
        easy_strerror = reinterpret_cast<decltype(easy_strerror)>(dlsym(handle, "curl_easy_strerror"));
        slist_append = reinterpret_cast<decltype(slist_append)>(dlsym(handle, "curl_slist_append"));
        slist_free_all = reinterpret_cast<decltype(slist_free_all)>(dlsym(handle, "curl_slist_free_all"));
        return easy_init && easy_setopt && easy_perform && easy_getinfo && easy_cleanup && easy_strerror
            && slist_append && slist_free_all;
    }
};

Curl* curl()
{
    static Curl* instance = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
        Curl* c = new Curl;
        if (c->load())
            instance = c;
        else
            delete c;
    });
    return instance;
}

size_t append(char* data, size_t size, size_t count, void* user)
{
    static_cast<std::string*>(user)->append(data, size * count);
    return size * count;
}

} // namespace

HttpResult http_post(const std::string& url, const std::string& body, const std::string& content_type,
    long timeout_seconds)
{
    HttpResult result;
    Curl* c = curl();
    if (!c) {
        result.error = "libcurl is not available";
        return result;
    }
    CURL* easy = c->easy_init();
    if (!easy) {
        result.error = "curl_easy_init failed";
        return result;
    }
    std::string header = "Content-Type: " + content_type;
    curl_slist* headers = c->slist_append(nullptr, header.c_str());
    c->easy_setopt(easy, kOptUrl, url.c_str());
    c->easy_setopt(easy, kOptPost, 1L);
    c->easy_setopt(easy, kOptPostFields, body.data());
    c->easy_setopt(easy, kOptPostFieldSize, static_cast<long>(body.size()));
    c->easy_setopt(easy, kOptHttpHeader, headers);
    c->easy_setopt(easy, kOptWriteFunction, &append);
    c->easy_setopt(easy, kOptWriteData, &result.body);
    c->easy_setopt(easy, kOptTimeout, timeout_seconds);
    c->easy_setopt(easy, kOptNoSignal, 1L);
    c->easy_setopt(easy, kOptFollowLocation, 0L);
    if (const char* ca = getenv("WPE_BROWSER_CA_FILE"))
        if (*ca)
            c->easy_setopt(easy, kOptCaInfo, ca);
    int code = c->easy_perform(easy);
    if (code == 0) {
        c->easy_getinfo(easy, kInfoResponseCode, &result.status);
        result.ok = result.status >= 200 && result.status < 300;
        if (!result.ok)
            result.error = "HTTP " + std::to_string(result.status);
    } else
        result.error = c->easy_strerror(code);
    c->slist_free_all(headers);
    c->easy_cleanup(easy);
    return result;
}

} // namespace ocdm_widevine
