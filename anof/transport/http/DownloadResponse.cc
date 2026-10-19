#include <anof/transport/http/DownloadResponse.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <muduox/net/http/HttpConditional.h>
#include <muduox/net/http/HttpRange.h>
#include <limits>
namespace anof::http {
using namespace muduox::net;
Parsed<HttpHeaders> copyDownloadConditions(const HttpHeaders& input) {
    HttpHeaders result;
    std::size_t bytes = 0, count = 0;
    for (const auto& field : input) {
        if (field.name != "if-match" && field.name != "if-none-match" && field.name != "range" && field.name != "if-range") continue;
        if (++count > 100 || field.value.size() > 65536 - bytes) return ProtocolError::InvalidArgument;
        bytes += field.value.size();
        result.append(field.name, field.value);
    }
    return result;
}
Result<DownloadResponse> downloadResponse(const DownloadDescriptor& value, const HttpHeaders& conditions, bool head, RequestId id) {
    if (!validateDirectoryName(value.name)) return ErrorCode::InternalError;
    const auto size = static_cast<std::uint64_t>(value.blob.size.value());
    if (size > std::numeric_limits<std::size_t>::max()) return ErrorCode::InternalError;
    auto tag = HttpEntityTag::fromOpaque("sha256-" + value.blob.sha256.toString());
    if (!tag) return ErrorCode::InternalError;
    HttpRepresentationMetadata metadata{true, tag, {}};
    const auto method = head ? HttpRequest::kHead : HttpRequest::kGet;
    const auto now = muduox::Timestamp::now();
    const auto condition = HttpConditional::evaluate(conditions, method, metadata, now);
    if (condition == HttpConditional::Result::InvalidCondition) return ErrorCode::InvalidArgument;
    if (condition == HttpConditional::Result::PreconditionFailed) return ErrorCode::PreconditionFailed;
    DownloadResponse response;
    response.headers.addHeader("ETag", tag->toString());
    response.headers.addHeader("Cache-Control", "private,no-store");
    response.headers.addHeader("X-Request-ID", id.toString());
    if (condition == HttpConditional::Result::NotModified) {
        response.headers.setStatusCode(HttpResponse::k304NotModified);
        response.headers.setStatusMessage("Not Modified");
        return response;
    }
    const auto range = HttpRange::evaluate(conditions, method, metadata, size, now);
    if (range.status == HttpRange::Status::Unsatisfiable) {
        response.headers.setStatusCode(static_cast<HttpResponse::HttpStatusCode>(416));
        response.headers.setStatusMessage("Range Not Satisfiable");
        response.headers.addHeader("Content-Range", HttpRange::unsatisfiedContentRange(size));
        response.length = 0;
        return response;
    }
    response.length = static_cast<std::size_t>(size);
    response.headers.setStatusCode(HttpResponse::k200Ok);
    response.headers.setStatusMessage("OK");
    if (range.status == HttpRange::Status::Resolved) {
        response.offset = range.range->offset;
        response.length = static_cast<std::size_t>(range.range->length);
        response.headers.setStatusCode(static_cast<HttpResponse::HttpStatusCode>(206));
        response.headers.setStatusMessage("Partial Content");
        response.headers.addHeader("Content-Range", *HttpRange::contentRange(*range.range, size));
    }
    response.content = !head && *response.length != 0;
    response.headers.setContentType("application/octet-stream");
    response.headers.addHeader("Accept-Ranges", "bytes");
    response.headers.addHeader("X-Content-Type-Options", "nosniff");
    // 全部 UTF-8 字节 percent 编码，避免引号、分号、反斜杠进入字段语法。
    std::string filename;
    constexpr char hex[] = "0123456789ABCDEF";
    for (const unsigned char byte : value.name) {
        filename += '%'; filename += hex[byte >> 4]; filename += hex[byte & 15];
    }
    response.headers.addHeader("Content-Disposition", "attachment; filename=\"download\"; filename*=UTF-8''" + filename);
    return response;
}
}  // namespace anof::http
