#include <apps/anof/RuntimeDependencies.h>
#include <sqlite3.h>
#include <curl/curl.h>
#include <cstdio>
int main() {
    const auto* curl = curl_version_info(CURLVERSION_NOW);
    if (!curl) return 1;
    std::printf("SQLite headers=%s runtime=%s; curl headers=%s runtime=%s\n",
                SQLITE_VERSION, sqlite3_libversion(), LIBCURL_VERSION, curl->version ? curl->version : "unknown");
    if (SQLITE_VERSION_NUMBER != sqlite3_libversion_number()) {
        std::fputs("SQLite header/library version mismatch\n", stderr);
        return 1;
    }
    if (const char* error = anof::app::runtimeDependencyFailure()) {
        std::fprintf(stderr, "%s\n", error);
        return 1;
    }
    return 0;
}
