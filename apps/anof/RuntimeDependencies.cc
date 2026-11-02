#include <apps/anof/RuntimeDependencies.h>
#include <anof/build/DependencyPolicy.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include <cstring>

namespace anof::app {
const char* runtimeDependencyFailure() noexcept {
    const int sqlite = sqlite3_libversion_number();
    if (sqlite < kMinimumSqliteVersion || sqlite >= 4000000 || !sqlite3_threadsafe())
        return "UNSUPPORTED_SQLITE_RUNTIME";
    const auto* curl = curl_version_info(CURLVERSION_NOW);
    if (!curl || curl->version_num < kMinimumCurlVersion || !(curl->features & CURL_VERSION_SSL) ||
        !(curl->features & CURL_VERSION_LARGEFILE)) return "UNSUPPORTED_CURL_RUNTIME";
    bool http = false, https = false;
    if (curl->protocols) {
        for (auto p = curl->protocols; *p; ++p) {
            http = http || std::strcmp(*p, "http") == 0;
            https = https || std::strcmp(*p, "https") == 0;
        }
    }
    return http && https ? nullptr : "UNSUPPORTED_CURL_RUNTIME";
}
}  // namespace anof::app
