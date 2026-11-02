#include <anof/application/maintenance/internal/FsckSupport.h>
#include <anof/infrastructure/config/TomlInstanceConfiguration.h>
#include <anof/infrastructure/crypto/SodiumTransferIdGenerator.h>
#include <anof/infrastructure/system/internal/InstanceFiles.h>
#include <anof/runtime/InstanceMaintenance.h>
#include <apps/anof/MaintenanceOutput.h>
#include <apps/anof/MaintenancePlanJson.h>
#include <apps/anof/OfflineCommands.h>
#include <apps/anof/ShutdownSignals.h>
#include <apps/anof/TerminalPassword.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <cerrno>
#include <cstdio>
#include <future>
namespace anof::app {
namespace {
using J = nlohmann::json;
namespace fs = anof::infrastructure::system::detail;
using maintenance::take;
MaintenanceFailure failure(const MaintenanceContext& c, MaintenanceError code, MaintenanceStage stage = MaintenanceStage::Report) { return maintenance::failure(c, code, stage); }
thread_local const std::atomic<bool>* outputCancellation = nullptr;
bool writeBytes(int fd, std::string_view bytes) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!bytes.empty()) {
        if (fd == STDOUT_FILENO && outputCancellation && outputCancellation->load()) return false;
        auto n = ::write(fd, bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            pollfd p{ fd, POLLOUT, 0 };
            ::poll(&p, 1, 25);
            continue;
        }
        if (n <= 0) return false;
        bytes.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}
struct OutputFlags {
    int out = ::fcntl(STDOUT_FILENO, F_GETFL), err = ::fcntl(STDERR_FILENO, F_GETFL);
    bool set() { return out >= 0 && err >= 0 && ::fcntl(STDOUT_FILENO, F_SETFL, out | O_NONBLOCK) == 0 && ::fcntl(STDERR_FILENO, F_SETFL, err | O_NONBLOCK) == 0; }
    ~OutputFlags() {
        if (out >= 0) ::fcntl(STDOUT_FILENO, F_SETFL, out);
        if (err >= 0) ::fcntl(STDERR_FILENO, F_SETFL, err);
    }
};
struct OutputCancellation {
    explicit OutputCancellation(const std::atomic<bool>& flag) { outputCancellation = &flag; }
    ~OutputCancellation() { outputCancellation = nullptr; }
};

bool validPath(const std::filesystem::path& p) { return p.is_absolute() && p.lexically_normal() == p && !p.filename().empty(); }
bool within(const std::filesystem::path& p, const std::filesystem::path& root) {
    auto a = p.begin(), b = root.begin();
    while (b != root.end()) {
        if (a == p.end() || *a++ != *b++) return false;
    }
    return true;
}
struct OutputFile {
    fs::OwnedFd parent, fd;
    OutputFile(const MaintenanceContext& c, const std::filesystem::path& path) {
        if (!validPath(path)) throw failure(c, MaintenanceError::UnsafePath);
        auto p = fs::openControlledDirectory(path.parent_path());
        if (!p) throw failure(c, MaintenanceError::UnsafePath);
        parent = std::move(p).value().rootFd;
        fd = fs::OwnedFd(::openat(parent.get(), path.filename().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
        if (fd.get() < 0) throw failure(c, errno == EEXIST ? MaintenanceError::AlreadyExists : MaintenanceError::IoError);
    }
    void finish(const MaintenanceContext& c) {
        if (::fsync(fd.get()) || ::fsync(parent.get())) throw failure(c, MaintenanceError::IoError);
    }
};
std::string input(const MaintenanceContext& c, const std::filesystem::path& path, std::size_t limit) {
    if (!validPath(path)) throw failure(c, MaintenanceError::UnsafePath, MaintenanceStage::Validate);
    auto parent = fs::openControlledDirectory(path.parent_path());
    if (!parent) throw failure(c, MaintenanceError::UnsafePath);
    auto file = fs::openPrivateFile(parent.value().rootFd.get(), path.filename().c_str(), false);
    if (!file) throw failure(c, MaintenanceError::UnsafePath);
    std::string text;
    char buffer[65536];
    for (;;) {
        auto n = ::read(file.value().get(), buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) throw failure(c, MaintenanceError::IoError);
        if (!n) break;
        if (static_cast<std::size_t>(n) > limit - text.size()) throw failure(c, MaintenanceError::InvalidPlan, MaintenanceStage::Validate);
        text.append(buffer, static_cast<std::size_t>(n));
    }
    return text;
}
J configurationData(const InstanceConfiguration& v) { return { { "source", "instance-file" }, { "listen_address", v.listenAddress }, { "port", v.port }, { "development_http", v.developmentHttp }, { "metadata_queue", v.metadataQueue }, { "http_in_flight", v.httpInFlight }, { "shutdown_grace_seconds", v.shutdownGraceSeconds }, { "disk_safety_min_bytes", std::to_string(v.diskSafety.minimum.value()) }, { "disk_safety_percent", v.diskSafety.percent.value() }, { "web_enabled", v.webEnabled }, { "public_origin", v.publicOrigin }, { "trusted_proxies", v.trustedProxies }, { "audit_retention_days", v.auditRetentionDays.value() }, { "audit_batch_size", v.auditBatchSize.value() } }; }
// finding先写私有临时流；结果返回后才有权声明实例身份/完整状态。不缓存全部finding。
struct Report {
    struct Close {
        void operator()(FILE* f) const noexcept {
            if (f) std::fclose(f);
        }
    };
    std::unique_ptr<FILE, Close> spool{ std::tmpfile() };
    std::size_t bytes = 0;
    explicit Report(const MaintenanceContext& c) {
        if (!spool) throw failure(c, MaintenanceError::IoError);
    }
    MaintenanceResult<void> finding(const MaintenanceContext& c, const MaintenanceFinding& f) {
        auto line = maintenanceFindingData(f).dump() + "\n";
        if (line.size() > kMaintenanceFindingBytes || line.size() > kMaintenanceReportBytes - bytes) return failure(c, MaintenanceError::CheckIncomplete);
        if (!writeBytes(::fileno(spool.get()), line)) return failure(c, MaintenanceError::IoError);
        bytes += line.size();
        return {};
    }
    void output(const MaintenanceContext& c, const FsckResult& r, InspectionMode mode, int fd) {
        J h = { { "type", "header" }, { "format", kMaintenanceReportFormat }, { "run_id", c.operationId.toString() }, { "instance_id", r.identity.instance ? J(r.identity.instance->toString()) : J(nullptr) }, { "mode", mode == InspectionMode::Full ? "full" : "quick" } };
        if (!writeBytes(fd, h.dump() + "\n") || ::lseek(::fileno(spool.get()), 0, SEEK_SET) < 0) throw failure(c, MaintenanceError::IoError);
        char b[65536];
        for (;;) {
            auto n = ::read(::fileno(spool.get()), b, sizeof b);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) throw failure(c, MaintenanceError::IoError);
            if (!n) break;
            if (!writeBytes(fd, { b, static_cast<std::size_t>(n) })) throw failure(c, MaintenanceError::IoError);
        }
        if (!writeBytes(fd, maintenanceSummaryData(r.report).dump() + "\n")) throw failure(c, MaintenanceError::IoError);
    }
};
}  // namespace
struct OfflineCommands::State {
    CLI::App *config, *show, *validate, *admin, *reset, *fsck, *backup, *restore;
    std::string root, work, report, planOut, applyPlan, backupDir, output, backupInput, configInput;
    bool full = false, repair = false, confirm = false, exclude = false;
    State(CLI::App& app) {
        config = app.add_subcommand("config", "Inspect local instance configuration");
        config->require_subcommand(1, 1);
        show = config->add_subcommand("show", "Show validated effective configuration");
        validate = config->add_subcommand("validate", "Validate configuration without database recovery");
        admin = app.add_subcommand("admin", "Offline administration");
        admin->require_subcommand(1, 1);
        reset = admin->add_subcommand("reset-password", "Reset the sole administrator using a terminal");
        fsck = app.add_subcommand("fsck", "Offline inspection and explicitly planned repair");
        backup = app.add_subcommand("backup", "Create a verified product backup in a new directory");
        restore = app.add_subcommand("restore", "Restore a verified backup into a new instance");
        for (auto cmd : { show, validate, reset, fsck, backup, restore }) cmd->add_option("--root", root)->required();
        for (auto cmd : { fsck, backup, restore }) cmd->add_option("--work-dir", work);
        fsck->add_flag("--full", full)->disable_flag_override();
        fsck->add_flag("--repair", repair)->disable_flag_override();
        fsck->add_option("--plan-out", planOut);
        fsck->add_option("--apply-plan", applyPlan);
        fsck->add_flag("--confirm-repair", confirm)->disable_flag_override();
        fsck->add_option("--backup-dir", backupDir);
        fsck->add_option("--report", report);
        backup->add_option("--output", output)->required();
        backup->add_flag("--exclude-quarantine", exclude)->disable_flag_override();
        restore->add_option("--backup", backupInput)->required();
        restore->add_option("--config", configInput)->required();
    }
    int execute(bool json, const MaintenanceContext& c, const PasswordInput* password) {
        OutputCancellation cancellation(c.cancelled);
        MaintenanceOutcome outcome = MaintenanceOutcome::NotStarted;
        try {
            runtime::InstanceMaintenance maintenance;
            auto emit = [&](J data) {
                data["operation_id"] = c.operationId.toString();
                data["outcome"] = "confirmed";
                data["stage"] = "report";
                data["report_status"] = "healthy";
                if (json) {
                    if (!writeBytes(STDOUT_FILENO, data.dump() + "\n")) throw failure(c, MaintenanceError::IoError);
                } else
                    for (auto it = data.begin(); it != data.end(); ++it)
                        if (!writeBytes(STDOUT_FILENO, it.key() + " = " + it.value().dump() + "\n")) throw failure(c, MaintenanceError::IoError);
            };
            if (*config) {
                auto value = take(maintenance.configuration(c, root));
                outcome = MaintenanceOutcome::Confirmed;
                emit(*show ? configurationData(value) : J{ { "valid", true }, { "source", "instance-file" }, { "ready", false } });
                return 0;
            }
            if (*reset) {
                take(maintenance.resetAdministrator(c, root, *password));
                outcome = MaintenanceOutcome::Confirmed;
                emit({ { "operation_id", c.operationId.toString() }, { "outcome", "confirmed" }, { "code", "ADMINISTRATOR_PASSWORD_RESET" } });
                return 0;
            }
            if (*backup) {
                auto value = take(maintenance.backups().backup(c, { root, output, work, exclude }));
                outcome = MaintenanceOutcome::Confirmed;
                const auto& e = value.manifest.exclusions;
                emit({ { "operation_id", c.operationId.toString() }, { "backup_id", value.manifest.id.toString() }, { "instance_id", value.manifest.instance.toString() }, { "outcome", "confirmed" }, { "artifact", value.artifact.string() }, { "exclusions", { { "quarantine_objects", std::to_string(e.quarantineObjects.value()) }, { "quarantine_bytes", std::to_string(e.quarantineBytes.value()) }, { "unreferenced_objects", std::to_string(e.unreferencedObjects.value()) }, { "unreferenced_bytes", std::to_string(e.unreferencedBytes.value()) }, { "uploads", std::to_string(e.uploads.value()) } } } });
                return 0;
            }
            if (*restore) {
                auto configValue = infrastructure::config::loadExplicitInstanceConfiguration(configInput);
                if (!configValue) throw failure(c, MaintenanceError::InvalidArgument, MaintenanceStage::Validate);
                auto value = take(maintenance.backups().restore(c, { backupInput, root, configValue.value() }));
                outcome = MaintenanceOutcome::Confirmed;
                emit({ { "operation_id", c.operationId.toString() }, { "instance_id", value.instance.toString() }, { "outcome", "confirmed" }, { "artifact", value.artifact.string() } });
                return 0;
            }
            std::optional<MaintenancePlan> plan;
            if (!applyPlan.empty()) {
                auto parsed = parseMaintenancePlan(input(c, applyPlan, kMaintenancePlanBytes));
                if (!parsed) throw failure(c, MaintenanceError::InvalidPlan, MaintenanceStage::Validate);
                plan = std::move(parsed).value();
            }
            // 输出目的预先独占创建，禁止用报告/计划路径覆盖源、输入或彼此。
            std::unique_ptr<OutputFile> reportFile, planFile;
            if (!report.empty()) reportFile = std::make_unique<OutputFile>(c, report);
            if (repair) planFile = std::make_unique<OutputFile>(c, planOut);
            Report details(c);
            FindingSink sink = [&](const auto& f) { return details.finding(c, f); };
            FsckResult result = plan ? [&] {auto applied=take(maintenance.fsck().apply(c,{root,work,backupDir,confirm},*plan,sink));outcome=MaintenanceOutcome::Confirmed;return std::move(applied.verification); }() : take(maintenance.fsck().inspect(c, { root, work, full ? InspectionMode::Full : InspectionMode::Quick, repair }, sink));
            if (!plan) outcome = MaintenanceOutcome::Confirmed;
            if (repair && result.plan) {
                auto bytes = maintenancePlanData(*result.plan).dump() + "\n";
                if (bytes.size() > kMaintenancePlanBytes) throw failure(c, MaintenanceError::InvalidPlan);
                if (!writeBytes(planFile->fd.get(), bytes)) throw failure(c, MaintenanceError::IoError);
                planFile->finish(c);
            }
            const auto mode = plan ? plan->mode : full ? InspectionMode::Full
                                                       : InspectionMode::Quick;
            if (reportFile) {
                details.output(c, result, mode, reportFile->fd.get());
                reportFile->finish(c);
            }
            if (json)
                details.output(c, result, mode, STDOUT_FILENO);
            else {
                std::string text = std::string("FSCK ") + maintenanceStatusName(result.report.status()) + "; report=" + report;
                if (repair) text += result.plan ? "; plan=" + planOut : "; no executable plan";
                if (!writeBytes(STDOUT_FILENO, text + "\n")) throw failure(c, MaintenanceError::IoError);
            }
            return result.report.status() == MaintenanceReportStatus::Healthy ? 0 : 4;
        } catch (MaintenanceFailure e) {
            const bool readOnly = *config || (*fsck && applyPlan.empty());
            const bool interruptedOutput = readOnly && c.cancelled.load() && e.code == MaintenanceError::IoError && e.stage == MaintenanceStage::Report;
            if (interruptedOutput) e.code = MaintenanceError::Cancelled;
            if (outcome == MaintenanceOutcome::Confirmed && e.outcome == MaintenanceOutcome::NotStarted) e.outcome = outcome;
            auto line = json ? maintenanceFailureData(e).dump() + "\n" : std::string("ANOF_MAINTENANCE code=") + maintenanceErrorName(e.code) + " stage=" + maintenanceStageName(e.stage) + " operation_id=" + c.operationId.toString() + " outcome=" + maintenanceOutcomeName(e.outcome) + "\n";
            if (!writeBytes(STDERR_FILENO, line)) return e.outcome == MaintenanceOutcome::Unknown ? 6 : 5;
            if (interruptedOutput) return 130;
            return maintenanceExitCode(e);
        } catch (...) {
            auto e = failure(c, MaintenanceError::InternalError);
            e.outcome = outcome;
            writeBytes(STDERR_FILENO, maintenanceFailureData(e).dump() + "\n");
            return maintenanceExitCode(e);
        }
    }
};
OfflineCommands::OfflineCommands(CLI::App& app) : state_(std::make_unique<State>(app)) {}
OfflineCommands::~OfflineCommands() = default;
bool OfflineCommands::selected() const { return *state_->config || *state_->admin || *state_->fsck || *state_->backup || *state_->restore; }
int OfflineCommands::run(bool json) {
    auto& s = *state_;
    try {
        for (const auto& path : { s.root, s.work, s.report, s.planOut, s.applyPlan, s.backupDir, s.output, s.backupInput, s.configInput }) (void)J(path).dump();
    } catch (...) {
        return 2;
    }
    if (!validPath(s.root) || (!s.work.empty() && !validPath(s.work))) return 2;
    if (*s.fsck) {
        if ((!s.applyPlan.empty() && (s.full || s.repair || !s.planOut.empty() || !s.confirm || s.backupDir.empty())) || (s.applyPlan.empty() && (s.confirm || !s.backupDir.empty())) || (s.repair != !s.planOut.empty())) return 2;
        for (const auto& p : { s.report, s.planOut, s.applyPlan })
            if (!p.empty() && (!validPath(p) || within(p, s.root))) return 2;
        if (!s.planOut.empty() && s.planOut == s.report) return 2;
    }
    auto ids = infrastructure::crypto::makeSodiumMaintenanceIdGenerator();
    if (!ids) return 5;
    auto generated = ids.value()->nextRun();
    if (!generated) return 5;
    auto operation = OperationId::parse(generated.value().toString()).value();
    if (s.work.empty()) s.work = std::filesystem::temp_directory_path();
    if (*s.fsck && !json && s.report.empty()) s.report = (std::filesystem::path(s.work) / ("anof-report-" + operation.toString() + ".jsonl")).string();
    std::optional<PasswordInput> password;
    if (*s.reset) {
        auto p = readResetPassword();
        if (auto error = std::get_if<TerminalError>(&p)) return *error == TerminalError::Interrupted ? 130 : *error == TerminalError::IoError ? 5
                                                                                                                                              : 2;
        password = std::move(std::get<PasswordInput>(p));
    }
    OutputFlags flags;
    if (!flags.set()) return 5;
    auto signals = ShutdownSignals::create();
    if (!signals) return 5;
    std::atomic<bool> cancelled{ false };
    MaintenanceContext c{ operation, cancelled };
    auto future = std::async(std::launch::async, [&] { return s.execute(json, c, password ? &*password : nullptr); });
    while (future.wait_for(std::chrono::milliseconds(25)) != std::future_status::ready) {
        auto signal = signals.value()->poll();
        if (!signal || signal.value()) cancelled.store(true);
    }
    return future.get();
}
}  // namespace anof::app
