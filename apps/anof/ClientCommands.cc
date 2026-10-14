#include <apps/anof/ClientCommands.h>
#include <apps/anof/ClientOutput.h>
#include <apps/anof/TerminalPassword.h>
#include <anof/client/CurlHttpTransport.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <CLI/CLI.hpp>
#include <array>
#include <charconv>
#include <atomic>
#include <csignal>
#include <stdexcept>
#include <sstream>
namespace anof::app {
namespace {
using namespace client;
using Json = nlohmann::json;
std::atomic_bool cancelled{false};
static_assert(std::atomic_bool::is_always_lock_free);
extern "C" void cancelClient(int) { cancelled.store(true,std::memory_order_relaxed); }
class ClientSignals {
public:
    ClientSignals() {
        cancelled.store(false,std::memory_order_relaxed);
        for (std::size_t i=0;i<signals_.size();++i) {
            struct sigaction action{}; sigemptyset(&action.sa_mask);
            action.sa_handler=signals_[i]==SIGPIPE?SIG_IGN:cancelClient;
            if (sigaction(signals_[i],&action,&previous_[i])!=0) { restore(); throw std::runtime_error("Signal setup failed"); }
            ++installed_;
        }
    }
    ~ClientSignals() { restore(); }
private:
    void restore() noexcept { while(installed_) { --installed_; (void)sigaction(signals_[installed_],&previous_[installed_],nullptr); } }
    const std::array<int,6> signals_{SIGINT,SIGTERM,SIGHUP,SIGQUIT,SIGTSTP,SIGPIPE};
    std::array<struct sigaction,6> previous_{};
    std::size_t installed_=0;
};
unsigned number(const std::string& text, unsigned max) {
    if (text.empty() || text.front()=='0') return 0;
    unsigned value=0;
    for(char c:text) { if(c<'0'||c>'9'||value>(max-static_cast<unsigned>(c-'0'))/10) return 0; value=value*10+static_cast<unsigned>(c-'0'); }
    return value<=max?value:0;
}
std::optional<std::int64_t> quantity(const std::string& text) {
    if (text.empty() || (text.size()>1 && text.front()=='0') || text.find_first_not_of("0123456789")!=std::string::npos) return {};
    std::int64_t value=0; auto result=std::from_chars(text.data(),text.data()+text.size(),value);
    if (result.ec!=std::errc{} || result.ptr!=text.data()+text.size()) return {};
    return value;
}
ClientError terminalError(TerminalError error) {
    if (error==TerminalError::Interrupted) return failure(ErrorKind::Interrupted);
    if (error==TerminalError::InvalidInput) return failure(ErrorKind::InvalidArgument);
    return failure(ErrorKind::LocalState,error==TerminalError::Unavailable?"TERMINAL_REQUIRED":"TERMINAL_IO_ERROR");
}
constexpr const char* helpText="Global --config-dir, --profile and --json precede the command. Secrets are read from /dev/tty only. Client exit codes: 0 success; 2 invalid arguments; 3 rejected/auth required; 4 read transport/protocol failure; 5 local state/output failure; 6 write outcome unknown (no automatic retry); 130 interrupted. init/serve retain separate exit semantics.";
CLI::App* sub(CLI::App& parent, const char* name, const char* description) { auto* app=parent.add_subcommand(name,description); app->footer(helpText); return app; }
}
struct ClientCommands::State {
    bool json=false, developmentHttp=false;
    std::string config, profile, name, server, login, displayName, id, space, parent, directoryName;
    std::string condition, quotaUser, bytes, nodes;
    std::string limit="100", cursor, maxUses="1", lifetime="604800";
    CLI::Option *configOption, *profileOption, *jsonOption, *inviteCursor, *childrenCursor, *userCursor, *quotaShowUser, *quotaSetUser;
    CLI::App *profiles, *profileAdd, *profileShow, *profileRemove, *profileList, *registerUser, *loginUser, *logout, *me,
        *inviteCreate, *inviteList, *inviteRevoke, *spaces, *children, *stat, *mkdir, *rename, *passwordChange, *userList, *userShow, *userEnable, *userDisable, *quotaShow, *quotaSet;
    explicit State(CLI::App& app) {
        configOption=app.add_option("--config-dir",config,"Absolute client configuration directory (client commands only)");
        profileOption=app.add_option("--profile",profile,"Explicit client profile (remote commands only)");
        jsonOption=app.add_flag("--json",json,"Machine-readable client output")->disable_flag_override();
        profiles=sub(app,"profile","Manage local remote-server profiles"); profiles->require_subcommand(1,1);
        profileAdd=sub(*profiles,"add","Add a profile without overwriting an existing one");
        profileAdd->add_option("NAME",name)->required(); profileAdd->add_option("--server",server)->required();
        profileAdd->add_flag("--development-http",developmentHttp,"Allow literal loopback HTTP only")->disable_flag_override();
        profileShow=sub(*profiles,"show","Show a redacted profile"); profileShow->add_option("NAME",name)->required();
        profileRemove=sub(*profiles,"remove","Remove local profile only; does not revoke the remote session"); profileRemove->add_option("NAME",name)->required();
        profileList=sub(*profiles,"list","List redacted profiles (maximum 100)");
        registerUser=sub(app,"register","Register with an invitation; no automatic login");
        registerUser->add_option("--login",login)->required(); registerUser->add_option("--display-name",displayName)->required();
        loginUser=sub(app,"login","Login and atomically save a bearer credential"); loginUser->add_option("--login",login)->required();
        logout=sub(app,"logout","Revoke the current session and clear local credentials even on remote failure");
        me=sub(app,"me","Read the authenticated user");
        auto* invites=sub(app,"invite","Manage registration invitations (administrator)"); invites->require_subcommand(1,1);
        inviteCreate=sub(*invites,"create","Create an invitation; secret is displayed once");
        inviteCreate->add_option("--max-uses",maxUses,"Canonical integer 1..10000; default 1");
        inviteCreate->add_option("--valid-for-seconds",lifetime,"Canonical integer 1..2592000; default 604800");
        inviteList=sub(*invites,"list","Read one invitation page"); inviteList->add_option("--limit",limit,"Canonical integer 1..200; default 100"); inviteCursor=inviteList->add_option("--cursor",cursor,"Opaque cursor from a preceding response");
        inviteRevoke=sub(*invites,"revoke","Revoke an invitation"); inviteRevoke->add_option("ID",id)->required();
        auto* account=sub(app,"account","Manage the authenticated account"); account->require_subcommand(1,1);
        rename=sub(*account,"rename","Change your display name with an explicit ETag");
        rename->add_option("--display-name",displayName)->required(); rename->add_option("--if-match",condition)->required();
        passwordChange=sub(*account,"password","Change password and revoke all sessions; on unknown outcome, verify or explicitly log in again");
        passwordChange->add_option("--if-match",condition)->required();
        auto* users=sub(app,"user","Manage users (administrator)"); users->require_subcommand(1,1);
        userList=sub(*users,"list","Read one user page"); userList->add_option("--limit",limit); userCursor=userList->add_option("--cursor",cursor);
        userShow=sub(*users,"show","Read user details and ETag"); userShow->add_option("--id",id)->required();
        userEnable=sub(*users,"enable","Enable a user; revoked sessions remain invalid");
        userDisable=sub(*users,"disable","Disable a user and revoke all their sessions");
        for (auto* cmd : {userEnable,userDisable}) { cmd->add_option("--id",id)->required(); cmd->add_option("--if-match",condition)->required(); }
        auto* quotas=sub(app,"quota","Manage space or public contribution quotas (administrator)"); quotas->require_subcommand(1,1);
        quotaShow=sub(*quotas,"show","Read quota and ETag"); quotaShow->add_option("--space-id",space)->required(); quotaShowUser=quotaShow->add_option("--user-id",quotaUser);
        quotaSet=sub(*quotas,"set","Replace both limits using an explicit ETag"); quotaSet->add_option("--space-id",space)->required(); quotaSetUser=quotaSet->add_option("--user-id",quotaUser);
        quotaSet->add_option("--bytes",bytes)->required(); quotaSet->add_option("--nodes",nodes)->required(); quotaSet->add_option("--if-match",condition)->required();
        spaces=sub(app,"spaces","List spaces, root IDs and quotas");
        children=sub(app,"ls","Read one directory page by UUID"); children->add_option("NODE_ID",id)->required();
        children->add_option("--limit",limit,"Canonical integer 1..200; default 100"); childrenCursor=children->add_option("--cursor",cursor,"Opaque cursor from a preceding response");
        stat=sub(app,"stat","Read node details by UUID"); stat->add_option("NODE_ID",id)->required();
        mkdir=sub(app,"mkdir","Create a directory using explicit space and parent UUIDs");
        mkdir->add_option("--space",space)->required(); mkdir->add_option("--parent",parent)->required(); mkdir->add_option("--name",directoryName)->required();
    }
};
ClientCommands::ClientCommands(CLI::App& app) : state_(std::make_unique<State>(app)) {}
ClientCommands::~ClientCommands() = default;
bool ClientCommands::hasGlobalOptions() const { const auto& s=*state_; return s.configOption->count() || s.profileOption->count() || s.jsonOption->count(); }
bool ClientCommands::validOptionOrder(int argc,char** argv) const {
    bool command=false;
    for (int i=1;i<argc;++i) {
        std::string_view v(argv[i]);
        const bool valueOption=v=="--config-dir"||v=="--profile";
        const bool global=valueOption||v=="--json"||v.substr(0,13)=="--config-dir="||v.substr(0,10)=="--profile="||v.substr(0,7)=="--json=";
        if (command && global) return false;
        if (!command && valueOption) { ++i; continue; }
        if (!command && !global && !v.empty() && v.front()!='-') command=true;
    }
    return true;
}
int ClientCommands::invalidArguments() const { return ClientOutput(state_->json).error(failure(ErrorKind::InvalidArgument)); }
int ClientCommands::help(const CLI::App& app, const CLI::CallForHelp& request) const {
    ClientOutput output(state_->json);
    try {
        ClientSignals signals;
        std::ostringstream text, errors;
        app.exit(request,text,errors);
        return output.help(text.str());
    } catch (...) { return output.error(failure(ErrorKind::LocalState)); }
}
int ClientCommands::run() {
    auto& s=*state_;
    ClientOutput output(s.json);
    try {
        ClientSignals signals;
        auto invalid=[&] { return output.error(failure(ErrorKind::InvalidArgument)); };
        if (*s.profiles ? s.profileOption->count()!=0 : !s.profileOption->count() || !ProfileStore::validName(s.profile)) return invalid();
        if (*s.profiles && !*s.profileList && !ProfileStore::validName(s.name)) return invalid();
        const unsigned pageLimit=number(s.limit,200), uses=number(s.maxUses,10000), seconds=number(s.lifetime,2592000);
        if ((*s.children||*s.inviteList||*s.userList) && (!pageLimit || ((s.childrenCursor->count()||s.inviteCursor->count()||s.userCursor->count()) && (s.cursor.empty()||s.cursor.size()>2048)))) return invalid();
        if (*s.inviteCreate && (!uses||!seconds)) return invalid();
        if ((*s.registerUser||*s.loginUser) && !LoginName::parse(s.login)) return invalid();
        if ((*s.registerUser||*s.rename) && !DisplayName::parse(s.displayName)) return invalid();
        if ((*s.children||*s.stat) && !NodeId::parse(s.id)) return invalid();
        if (*s.inviteRevoke && !InviteId::parse(s.id)) return invalid();
        if (*s.mkdir && (!SpaceId::parse(s.space)||!NodeId::parse(s.parent)||!validateDirectoryName(s.directoryName))) return invalid();
        const bool accountWrite=*s.rename||*s.passwordChange||*s.userEnable||*s.userDisable||*s.quotaSet;
        auto tag=AccountTag::parse(s.condition);
        if (accountWrite && !tag) return invalid();
        if ((*s.userShow||*s.userEnable||*s.userDisable) && !UserId::parse(s.id)) return invalid();
        if ((*s.quotaShow||*s.quotaSet) && (!SpaceId::parse(s.space) || ((s.quotaShowUser->count()||s.quotaSetUser->count()) && !UserId::parse(s.quotaUser)))) return invalid();
        const auto quotaBytes=quantity(s.bytes), quotaNodes=quantity(s.nodes);
        if (*s.quotaSet && (!quotaBytes||!quotaNodes)) return invalid();
        auto origin=Origin::parse(s.server,s.developmentHttp);
        if (*s.profileAdd && !origin) return invalid();
        auto path=ProfileStore::resolveDirectory(s.configOption->count()?std::optional<std::string>(s.config):std::nullopt);
        if (!path) return output.error(path.error());
        ProfileStore store(path.value());
        if (cancelled.load()) return output.error(failure(ErrorKind::Interrupted));
        if (*s.profiles) {
            output.profile(*s.profileList?std::nullopt:std::optional<std::string>(s.name));
            if (*s.profileAdd) {
                auto result=store.add(s.name,origin.value()); if (!result) return output.error(result.error());
                return output.success(clientData(ProfileSummary{s.name,origin.value().value(),s.developmentHttp,{}}));
            }
            if (*s.profileRemove) { auto result=store.remove(s.name); if (!result) return output.error(result.error()); return output.success(nullptr); }
            if (*s.profileShow) { auto result=store.show(s.name); if (!result) return output.error(result.error()); return output.success(clientData(result.value())); }
            auto result=store.list(); if (!result) return output.error(result.error()); return output.success(clientData(result.value()));
        }
        output.profile(s.profile);
        auto held=store.lock(s.profile); if (!held) return output.error(held.error());
        if (cancelled.load(std::memory_order_relaxed)) return output.error(failure(ErrorKind::Interrupted));
        CurlHttpTransport transport;
        ApiClient api(transport,held.value(),&cancelled);
        auto emit=[&](auto result, LocalState local=LocalState::Unchanged) -> int {
            if (!result) return output.error(result.error());
            output.remote=RemoteOutcome::ConfirmedSuccess; output.local=local; output.confirmed=result.value().meta;
            auto data=clientData(result.value().data);
            if (result.value().meta.httpStatus==201 && *s.mkdir) data.erase("etag");
            if (*s.spaces) data=Json{{"items",std::move(data)}};
            return output.success(data,result.value().meta);
        };
        if (*s.loginUser) {
            if (held.value().profile().credential) return output.error(failure(ErrorKind::LocalState,"CREDENTIAL_ALREADY_PRESENT"));
            auto password=readLoginPassword(&cancelled); if (auto* error=std::get_if<TerminalError>(&password)) return output.error(terminalError(*error));
            return emit(api.login(LoginName::parse(s.login).value(),std::get<PasswordInput>(password)),LocalState::CredentialSaved);
        }
        if (*s.registerUser) {
            auto secrets=readRegistrationSecrets(&cancelled); if (auto* error=std::get_if<TerminalError>(&secrets)) return output.error(terminalError(*error));
            const auto& value=std::get<RegistrationSecrets>(secrets);
            return emit(api.registerUser(LoginName::parse(s.login).value(),DisplayName::parse(s.displayName).value(),value.password,value.invite));
        }
        if (*s.logout) {
            const bool hadCredential=held.value().profile().credential.has_value();
            auto result=api.logout(); if (!result) return output.error(result.error());
            output.local=hadCredential?LocalState::CredentialCleared:LocalState::Unchanged;
            output.confirmed=result.value().meta; if(output.confirmed) output.remote=RemoteOutcome::ConfirmedSuccess;
            return output.success(nullptr,result.value().meta);
        }
        if (*s.rename) return emit(api.rename(DisplayName::parse(s.displayName).value(),tag.value()));
        if (*s.passwordChange) {
            output.passwordChange=true;
            if (!held.value().profile().credential) return output.error(failure(ErrorKind::AuthenticationRequired));
            auto secrets=readPasswordChangeSecrets(&cancelled);
            if (auto* error=std::get_if<TerminalError>(&secrets)) return output.error(terminalError(*error));
            const auto& value=std::get<PasswordChangeSecrets>(secrets);
            return emit(api.changePassword(value.current,value.next,tag.value()),LocalState::CredentialCleared);
        }
        if (*s.userShow) return emit(api.getUser(UserId::parse(s.id).value()));
        if (*s.userEnable||*s.userDisable) return emit(api.setUserEnabled(UserId::parse(s.id).value(),static_cast<bool>(*s.userEnable),tag.value()));
        if (*s.quotaShow||*s.quotaSet) {
            const QuotaTarget target{SpaceId::parse(s.space).value(), (s.quotaShowUser->count()||s.quotaSetUser->count())?std::optional<UserId>(UserId::parse(s.quotaUser).value()):std::nullopt};
            if (*s.quotaShow) return emit(api.getQuota(target));
            return emit(api.setQuotaLimit(target,{ByteCount::from(*quotaBytes).value(),NodeCount::from(*quotaNodes).value()},tag.value()));
        }
        if (*s.me) return emit(api.me());
        if (*s.inviteCreate) {
            auto result=api.createInvite(uses,seconds); if (!result) return output.error(result.error());
            output.remote=RemoteOutcome::ConfirmedSuccess; output.confirmed=result.value().meta;
            return output.success(clientData(result.value().data.invite),result.value().meta,result.value().data.token.view());
        }
        const PageRequest page{pageLimit,(s.childrenCursor->count()||s.inviteCursor->count()||s.userCursor->count())?std::optional<std::string>(s.cursor):std::nullopt};
        if (*s.userList) return emit(api.listUsers(page));
        if (*s.inviteList) return emit(api.listInvites(page));
        if (*s.inviteRevoke) return emit(api.revokeInvite(InviteId::parse(s.id).value()));
        if (*s.spaces) return emit(api.spaces());
        if (*s.children) return emit(api.children(NodeId::parse(s.id).value(),page));
        if (*s.stat) return emit(api.stat(NodeId::parse(s.id).value()));
        if (*s.mkdir) return emit(api.mkdir(SpaceId::parse(s.space).value(),NodeId::parse(s.parent).value(),s.directoryName));
        return invalid();
    } catch (...) { return output.error(output.outputFailure()); }
}
}  // namespace anof::app
