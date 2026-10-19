#pragma once
#include <anof/client/HttpTransport.h>

#include <nlohmann/json.hpp>
namespace anof::client {
enum class TransferOperation {
    Create,
    Show,
    List,
    Complete,
    Cancel,
    Storage,
    SetStorage,
    Patch,
    UploadHead,
    ContentHead
};
// 仅decoder能构造；data只含逐字段验证并重建的wire DTO，不转发任意远端JSON。
class TransferReply {
public:
    const nlohmann::json& data() const { return data_; }
    const ResponseMeta& meta() const { return meta_; }

private:
    TransferReply(nlohmann::json data, ResponseMeta meta) : data_(std::move(data)), meta_(std::move(meta)) {}
    nlohmann::json data_;
    ResponseMeta meta_;
    friend ClientResult<TransferReply> decodeTransfer(TransferOperation, const HttpResponse&, unsigned);
};
ClientResult<TransferReply> decodeTransfer(TransferOperation, const HttpResponse&, unsigned limit = 200);
}  // namespace anof::client
