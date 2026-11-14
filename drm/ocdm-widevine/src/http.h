// Minimal HTTPS POST through the system libcurl, loaded at run time (libcurl.so.4 on webOS 5+, libcurl.so.5 before).
// Used once per device for Widevine provisioning.
#pragma once

#include <string>

namespace ocdm_widevine {

struct HttpResult {
    bool ok = false;        // transport succeeded and the server answered 2xx
    long status = 0;        // HTTP status, 0 if no answer
    std::string body;
    std::string error;
};

// POSTs |body| to |url| with |content_type|. Uses $WPE_BROWSER_CA_FILE as the CA bundle when set.
HttpResult http_post(const std::string& url, const std::string& body, const std::string& content_type,
    long timeout_seconds);

} // namespace ocdm_widevine
