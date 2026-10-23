#pragma once
#include <anof/application/drive/DriveCommands.h>
#include <anof/transport/http/IdentityRequest.h>
namespace anof::http {
Parsed<DriveCondition> parseDriveCondition(const muduox::net::HttpHeaders&);
Parsed<UpdateNodeCommand> parseUpdateNode(std::string_view, NodeId, DriveCondition);
Parsed<CopyFileCommand> parseCopyFile(std::string_view, NodeId, DriveCondition);
Parsed<RestoreTrashCommand> parseRestoreTrash(std::string_view, TrashId, DriveCondition);
}  // namespace anof::http
