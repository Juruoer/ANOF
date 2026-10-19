#include <anof/client/TransferClient.h>
#include <anof/domain/transfer/TransferValues.h>
#include <apps/anof/TransferCommands.h>

#include <CLI/CLI.hpp>
namespace anof::app {
using namespace client;
struct TransferCommands::State {
    std::string file, space, parent, name, id, output, cursor, limit = "100", budget, tag;
    CLI::App *upload, *download, *storage, *uploadResume, *downloadResume, *show, *list, *complete, *cancel,
        *storageShow, *storageSet;
    explicit State(CLI::App& app) {
        upload = app.add_subcommand("upload",
                                    "Upload a regular absolute-path file; uncertain writes require explicit resume");
        upload->add_option("FILE", file);
        upload->add_option("--space", space);
        upload->add_option("--parent", parent);
        upload->add_option("--name", name);
        uploadResume = upload->add_subcommand("resume", "Query the server before resuming a saved upload");
        uploadResume->add_option("TRANSFER_ID", id)->required();
        show = upload->add_subcommand("show", "Show one remote upload");
        show->add_option("UPLOAD_ID", id)->required();
        list = upload->add_subcommand("list", "Read one page of your uploads");
        list->add_option("--limit", limit);
        list->add_option("--cursor", cursor);
        complete =
            upload->add_subcommand("complete", "Accept publication once; 202 does not promise published content");
        complete->add_option("UPLOAD_ID", id)->required();
        complete->add_option("--parent", parent);
        complete->add_option("--name", name);
        cancel = upload->add_subcommand("cancel", "Cancel once; cleanup may remain in progress");
        cancel->add_option("UPLOAD_ID", id)->required();
        upload->require_subcommand(0, 1);
        download = app.add_subcommand("download", "Download to a nonexistent absolute-path destination");
        download->add_option("NODE_ID", id);
        download->add_option("--output", output);
        downloadResume = download->add_subcommand("resume", "Resume a saved partial download");
        downloadResume->add_option("TRANSFER_ID", id)->required();
        download->require_subcommand(0, 1);
        storage = app.add_subcommand("storage", "Manage instance content budget (administrator)");
        storage->require_subcommand(1, 1);
        storageShow = storage->add_subcommand("show", "Read storage accounting and ETag");
        storageSet = storage->add_subcommand("set", "Set content budget with an explicit ETag");
        storageSet->add_option("--content-budget-bytes", budget)->required();
        storageSet->add_option("--if-match", tag)->required();
    }
};
TransferCommands::TransferCommands(CLI::App& app) : state_(std::make_unique<State>(app)) {}
TransferCommands::~TransferCommands() = default;
std::optional<int> TransferCommands::run(const std::string& directory, LockedProfile& profile,
                                         CurlHttpTransport& transport, const std::atomic_bool* cancelled,
                                         ClientOutput& output) {
    auto& s = *state_;
    if (!*s.upload && !*s.download && !*s.storage) return {};
    auto invalid = [&] { return output.error(failure(ErrorKind::InvalidArgument)); };
    TransferClient client(transport, transport, profile, directory, cancelled);
    auto emit = [&](ClientResult<TransferReply> result) {
        if (!result) return output.error(result.error());
        output.remote = RemoteOutcome::ConfirmedSuccess;
        output.confirmed = result.value().meta();
        return output.success(result.value().data(), result.value().meta());
    };
    auto transferred = [&](ClientResult<TransferResult> result) {
        if (!result) return output.error(result.error());
        auto& value = result.value();
        output.transferId = value.data.at("transfer_id").get<std::string>();
        output.remote = value.remote;
        output.local = value.local;
        output.confirmed = value.meta;
        return output.success(value.data, value.meta);
    };
    if (*s.storageShow) return emit(client.storage());
    if (*s.storageSet) {
        auto n = parseTransferDecimal(s.budget);
        if (!n) return invalid();
        return emit(client.setStorage(n.value(), s.tag));
    }
    if (*s.uploadResume || *s.downloadResume) {
        if (!s.file.empty() || !s.output.empty() || !s.space.empty() || !s.parent.empty() || !s.name.empty())
            return invalid();
        return transferred(client.resume(s.id, static_cast<bool>(*s.downloadResume)));
    }
    if (*s.list) {
        auto n = parseTransferDecimal(s.limit);
        if (!n || n.value().value() < 1 || n.value().value() > 200) return invalid();
        return emit(client.list({ static_cast<unsigned>(n.value().value()),
                                  s.cursor.empty() ? std::nullopt : std::optional<std::string>(s.cursor) }));
    }
    if (*s.show || *s.complete || *s.cancel) {
        auto id = UploadId::parse(s.id);
        if (!id || id.value().toString() != s.id) return invalid();
        if (*s.show) return emit(client.show(id.value()));
        if (*s.cancel) return emit(client.cancel(id.value()));
        if (s.parent.empty() != s.name.empty()) return invalid();
        std::optional<std::pair<NodeId, std::string>> target;
        if (!s.parent.empty()) {
            auto parent = NodeId::parse(s.parent);
            if (!parent || parent.value().toString() != s.parent) return invalid();
            target = std::make_pair(parent.value(), s.name);
        }
        return emit(client.complete(id.value(), target));
    }
    if (*s.download) {
        auto id = NodeId::parse(s.id);
        if (!id || id.value().toString() != s.id) return invalid();
        return transferred(client.download(id.value(), s.output));
    }
    auto space = SpaceId::parse(s.space);
    auto parent = NodeId::parse(s.parent);
    if (!space || !parent || space.value().toString() != s.space || parent.value().toString() != s.parent)
        return invalid();
    return transferred(client.upload(s.file, space.value(), parent.value(), s.name));
}
}  // namespace anof::app
