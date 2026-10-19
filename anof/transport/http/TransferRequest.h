#pragma once
#include <anof/application/transfer/TransferCommands.h>
#include <anof/transport/http/IdentityRequest.h>
namespace anof::http {
Parsed<UploadId> parseUploadId(std::string_view);
Parsed<CreateUploadCommand> parseCreateUpload(std::string_view);
Parsed<CompleteUploadCommand> parseCompleteUpload(std::string_view, UploadId);
Parsed<StorageCondition> parseStorageCondition(const muduox::net::HttpHeaders&);
Parsed<SetStorageBudgetCommand> parseStorageUpdate(std::string_view, StorageCondition);
Parsed<PreparePatchCommand> parsePatchHeaders(const muduox::net::HttpHeaders&, UploadId);
}  // namespace anof::http
