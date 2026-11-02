// 仅供子进程启动前加载的故障注入，不接触真实实例。
#if defined(ANOF_TEST_OLD_SQLITE)
extern "C" int sqlite3_libversion_number() { return 3051002; }
#else
#include <curl/curl.h>
extern "C" curl_version_info_data* curl_version_info(CURLversion) {
    static curl_version_info_data info{}; // 无HTTPS能力的运行库。
    info.version_num = LIBCURL_VERSION_NUM;
    return &info;
}
#endif
