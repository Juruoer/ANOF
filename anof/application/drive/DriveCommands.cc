#include <anof/application/drive/DriveCommands.h>
#include <anof/domain/drive/DirectoryPolicy.h>
namespace anof {
Result<void> validateDriveCommand(const UpdateNodeCommand& c) {
    if (!c.parent && !c.name) return ErrorCode::InvalidArgument;
    const auto* expected = std::get_if<NodeCondition>(&c.condition);
    if (!expected || expected->id != c.id) return ErrorCode::PreconditionFailed;
    if (c.name) return validateDirectoryName(*c.name);
    return {};
}
Result<void> validateDriveCommand(const CopyFileCommand& c) {
    const auto* expected = std::get_if<NodeCondition>(&c.condition);
    if (!expected || expected->id != c.id) return ErrorCode::PreconditionFailed;
    return validateDirectoryName(c.name);
}
Result<void> validateDriveCommand(const RestoreTrashCommand& c) {
    const auto* expected = std::get_if<TrashCondition>(&c.condition);
    if (!expected || expected->id != c.id) return ErrorCode::PreconditionFailed;
    if (c.name) return validateDirectoryName(*c.name);
    return {};
}
Result<void> validateDriveCommand(UserId user, const SearchNodesCommand& c) {
    if (c.after && (c.after->user != user || c.after->space != c.space || c.after->text.value() != c.text.value()))
        return ErrorCode::InvalidArgument;
    return {};
}
Result<void> validateDriveCommand(UserId user, const ListTrashCommand& c) {
    if (c.after && (c.after->user != user || c.after->space != c.space || c.after->root != c.root))
        return ErrorCode::InvalidArgument;
    return {};
}
}  // namespace anof
