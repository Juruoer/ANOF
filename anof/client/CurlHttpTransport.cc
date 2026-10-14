#include <anof/client/CurlHttpTransport.h>
#include <curl/curl.h>
#include <arpa/inet.h>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <stdexcept>

namespace anof::client {
namespace {
bool asciiControl(unsigned char c) { return c <= 32 || c >= 127; }
std::string lower(std::string value) { for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 'a' - 'A'); return value; }
struct CurlGlobal { CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT); ~CurlGlobal() { if (result == CURLE_OK) curl_global_cleanup(); } };
struct Headers {
    curl_slist* value = nullptr;
    ~Headers() {
        // curl_slist 是本适配器持有的 Authorization 副本。
        for (auto* p = value; p; p = p->next) { volatile char* s = p->data; for (std::size_t i = 0, n = std::strlen(p->data); i < n; ++i) s[i] = 0; }
        curl_slist_free_all(value);
    }
    bool add(const char* text) { auto* next = curl_slist_append(value, text); if (!next) return false; value = next; return true; }
};
struct Transfer {
    HttpResponse response;
    std::size_t headerBytes = 0;
    bool invalid = false;
    const std::atomic_bool* cancel;
    bool headersComplete = false;
};
std::size_t bodyCallback(char* data, std::size_t size, std::size_t count, void* context) noexcept {
    auto& state = *static_cast<Transfer*>(context);
    if (size && count > kResponseLimit / size) { state.invalid = true; return 0; }
    const auto bytes = size * count;
    if (!state.response.body.append({data, bytes})) { state.invalid = true; return 0; }
    return bytes;
}
std::size_t headerCallback(char* data, std::size_t size, std::size_t count, void* context) noexcept {
    auto& state = *static_cast<Transfer*>(context);
    try {
        if (size && count > kHeaderLimit / size) { state.invalid = true; return 0; }
        const auto bytes = size * count;
        if (bytes > kHeaderLimit - state.headerBytes) { state.invalid = true; return 0; }
        state.headerBytes += bytes;
        std::string_view line(data, bytes);
        if (line.substr(0, 5) == "HTTP/") { state.response.headers.clear(); state.headersComplete = false; return bytes; }
        if (line == "\r\n") { state.headersComplete = true; return bytes; }
        if (state.headersComplete) { state.invalid = true; return 0; }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) { state.invalid = true; return 0; }
        auto name = lower(std::string(line.substr(0, colon)));
        line.remove_prefix(colon + 1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        if (std::any_of(line.begin(), line.end(), [](unsigned char c) { return c < 32 || c == 127; })) { state.invalid = true; return 0; }
        const bool critical = name == "content-type" || name == "content-encoding" || name == "content-length" || name == "x-request-id" || name == "etag" || name == "location" || name == "transfer-encoding";
        // 不保留 Set-Cookie 或未知头，避免将响应中的秘密扩散到通用字符串。
        if (critical && !state.response.headers.emplace(std::move(name), line).second) { state.invalid = true; return 0; }
        return bytes;
    } catch (...) { state.invalid = true; return 0; }
}
int progressCallback(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    const auto* cancel = static_cast<Transfer*>(context)->cancel;
    return cancel && cancel->load(std::memory_order_relaxed) ? 1 : 0;
}
}  // namespace
ClientResult<Origin> Origin::parse(std::string_view text, bool developmentHttp) {
    auto bad = failure(ErrorKind::InvalidArgument);
    if (text.empty() || text.size() > 2048 || std::any_of(text.begin(), text.end(), asciiControl)) return bad;
    const auto split = text.find("://");
    if (split == std::string_view::npos) return bad;
    auto scheme = lower(std::string(text.substr(0, split)));
    if ((scheme != "http" && scheme != "https") || (scheme == "https" && developmentHttp)) return bad;
    auto authority = text.substr(split + 3);
    if (!authority.empty() && authority.back() == '/') authority.remove_suffix(1);
    if (authority.empty() || authority.find_first_of("/@?#\\%") != std::string_view::npos) return bad;
    std::string host;
    std::string_view port;
    if (authority.front() == '[') {
        const auto end = authority.find(']');
        if (end == std::string_view::npos) return bad;
        host = lower(std::string(authority.substr(0, end + 1)));
        in6_addr address{};
        auto literal = host.substr(1, host.size() - 2);
        if (inet_pton(AF_INET6, literal.c_str(), &address) != 1) return bad;
        if (end + 1 != authority.size()) { if (authority[end + 1] != ':') return bad; port = authority.substr(end + 2); if (port.empty()) return bad; }
    } else {
        const auto colon = authority.find(':');
        host = lower(std::string(authority.substr(0, colon)));
        if (host.empty() || host.size() > 253) return bad;
        std::size_t start = 0;
        while (start < host.size()) {
            auto end = host.find('.', start); if (end == std::string::npos) end = host.size();
            if (end == start || end - start > 63 || host[start] == '-' || host[end - 1] == '-') return bad;
            for (auto i = start; i < end; ++i) if (!(host[i] >= 'a' && host[i] <= 'z') && !(host[i] >= '0' && host[i] <= '9') && host[i] != '-') return bad;
            start = end + 1;
        }
        if (host.back() == '.') return bad;
        if (colon != std::string_view::npos) { port = authority.substr(colon + 1); if (port.empty()) return bad; }
    }
    if (scheme == "http" && (!developmentHttp || (host != "127.0.0.1" && host != "[::1]"))) return bad;
    unsigned number = scheme == "https" ? 443 : 80;
    if (!port.empty()) {
        const auto parsed = std::from_chars(port.data(), port.data() + port.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != port.data() + port.size() || number == 0 || number > 65535) return bad;
    }
    auto origin = scheme + "://" + host;
    if (number != (scheme == "https" ? 443U : 80U)) origin += ":" + std::to_string(number);
    return Origin(std::move(origin), developmentHttp);
}
CurlHttpTransport::CurlHttpTransport(long connectTimeoutMs, long timeoutMs) : connectTimeoutMs_(connectTimeoutMs), timeoutMs_(timeoutMs) {
    if (connectTimeoutMs <= 0 || connectTimeoutMs > 5000 || timeoutMs <= 0 || timeoutMs > 35000) throw std::invalid_argument("Invalid client timeout");
}
ClientResult<HttpResponse> CurlHttpTransport::perform(const Origin& origin, const HttpRequest& request, const std::atomic_bool* cancel) {
    if (cancel && cancel->load(std::memory_order_relaxed)) return failure(ErrorKind::Interrupted);
    if (request.path.substr(0, 8) != "/api/v1/" || request.path.find_first_of("\r\n#\\") != std::string::npos ||
        std::any_of(request.path.begin(), request.path.end(), asciiControl) || request.path.size() > 8192 || request.body.view().size() > kRequestLimit ||
        ((request.method == Method::Get || request.method == Method::Delete) && !request.body.view().empty())) return failure(ErrorKind::InvalidArgument);
    bool started = false;
    try {
        static CurlGlobal global;
        if (global.result != CURLE_OK) return failure(ErrorKind::Transport);
        Transfer state{{}, 0, false, cancel};
        Headers headers;
        // easy 先销毁，回调上下文及借用的头链表随后释放。
        std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
        if (!curl) return failure(ErrorKind::Transport);
        if (!headers.add("Accept: application/json") || !headers.add("Accept-Encoding: identity") || !headers.add("Expect:")) return failure(ErrorKind::Transport);
        if ((request.method == Method::Post || request.method == Method::Patch || request.method == Method::Put) && !headers.add("Content-Type: application/json; charset=utf-8")) return failure(ErrorKind::Transport);
        if (request.ifMatch && !headers.add(("If-Match: " + request.ifMatch->value()).c_str())) return failure(ErrorKind::Transport);
        if (request.bearer) {
            if (!SecretToken::session(request.bearer->view())) return failure(ErrorKind::InvalidArgument);
            SecureBuffer authorization(128);
            authorization.append("Authorization: Bearer "); authorization.append(request.bearer->view()); authorization.append(std::string_view("\0", 1));
            if (!headers.add(authorization.view().data())) return failure(ErrorKind::Transport);
        }
        auto set = [&](auto option, auto value) { if (curl_easy_setopt(curl.get(), option, value) != CURLE_OK) throw std::runtime_error("Curl setup failed"); };
        const auto url = origin.value() + request.path;
        set(CURLOPT_URL, url.c_str()); set(CURLOPT_PROTOCOLS_STR, "http,https");
        set(CURLOPT_FOLLOWLOCATION, 0L); set(CURLOPT_MAXREDIRS, 0L);
        set(CURLOPT_PROXY, ""); set(CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
        set(CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_NONE));
        set(CURLOPT_SSL_VERIFYPEER, 1L); set(CURLOPT_SSL_VERIFYHOST, 2L);
        set(CURLOPT_NOSIGNAL, 1L); set(CURLOPT_CONNECTTIMEOUT_MS, connectTimeoutMs_); set(CURLOPT_TIMEOUT_MS, timeoutMs_);
        set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
        set(CURLOPT_HTTP_CONTENT_DECODING, 0L); set(CURLOPT_FRESH_CONNECT, 1L); set(CURLOPT_FORBID_REUSE, 1L);
        set(CURLOPT_HTTPHEADER, headers.value);
        set(CURLOPT_WRITEFUNCTION, &bodyCallback); set(CURLOPT_WRITEDATA, &state);
        set(CURLOPT_HEADERFUNCTION, &headerCallback); set(CURLOPT_HEADERDATA, &state);
        set(CURLOPT_NOPROGRESS, 0L); set(CURLOPT_XFERINFOFUNCTION, &progressCallback); set(CURLOPT_XFERINFODATA, &state);
        if (request.method == Method::Post || request.method == Method::Patch || request.method == Method::Put) {
            set(CURLOPT_POST, 1L); set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.view().size()));
            set(CURLOPT_POSTFIELDS, request.body.view().empty() ? "" : request.body.view().data());
            if (request.method != Method::Post) set(CURLOPT_CUSTOMREQUEST, request.method == Method::Patch ? "PATCH" : "PUT");
        } else if (request.method == Method::Delete) set(CURLOPT_CUSTOMREQUEST, "DELETE");
        if (cancel && cancel->load(std::memory_order_relaxed)) return failure(ErrorKind::Interrupted);
        started = true;
        const auto result = curl_easy_perform(curl.get());
        if (result != CURLE_OK) {
            auto error = failure(request.writes() ? ErrorKind::OutcomeUnknown : state.invalid ? ErrorKind::Protocol : result == CURLE_ABORTED_BY_CALLBACK ? ErrorKind::Interrupted : ErrorKind::Transport);
            error.remote = RemoteOutcome::Unknown;
            return error;
        }
        long status = 0;
        if (curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status) != CURLE_OK || status < 100 || status > 599) return failure(request.writes() ? ErrorKind::OutcomeUnknown : ErrorKind::Protocol);
        state.response.status = static_cast<int>(status);
        return std::move(state.response);
    } catch (...) {
        auto error = failure(started && request.writes() ? ErrorKind::OutcomeUnknown : ErrorKind::Transport);
        if (started) error.remote = RemoteOutcome::Unknown;
        return error;
    }
}
}  // namespace anof::client
