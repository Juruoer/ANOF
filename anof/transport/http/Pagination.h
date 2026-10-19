#pragma once
#include <anof/application/transfer/TransferCommands.h>

#include <anof/transport/http/IdentityRequest.h>
#include <anof/application/identity/AccountCommands.h>

namespace anof::http {
inline constexpr std::size_t kMaxCursorBytes = 2048;
inline constexpr std::size_t kMaxDecodedCursorBytes = 1536;
Parsed<ChildrenCursor> decodeChildrenCursor(std::string_view cursor);
Parsed<InviteCursor> decodeInviteCursor(std::string_view cursor);
Result<std::string> encodeCursor(const ChildrenCursor& cursor);
Result<std::string> encodeCursor(const InviteCursor& cursor);
Parsed<ListChildrenCommand> parseListChildren(std::string_view parent, std::string_view query);
Parsed<ListInvitesCommand> parseListInvites(std::string_view query);
Parsed<UserCursor> decodeUserCursor(std::string_view cursor);
Result<std::string> encodeCursor(const UserCursor& cursor);
Parsed<ListUsersCommand> parseListUsers(std::string_view query);
Parsed<UploadCursor> decodeUploadCursor(std::string_view);
Result<std::string> encodeCursor(const UploadCursor&);
Parsed<ListUploadsCommand> parseListUploads(std::string_view);
Result<UtcTime> parseUtcTimestamp(std::string_view text);
}  // namespace anof::http
