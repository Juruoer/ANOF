#include <apps/anof/DriveCommands.h>
#include <anof/client/DriveClient.h>
#include <anof/domain/drive/DirectoryPolicy.h>
#include <anof/domain/drive/TreePolicy.h>
#include <anof/domain/transfer/TransferValues.h>
#include <CLI/CLI.hpp>
namespace anof::app {
using namespace client;
struct DriveCommands::State {
    std::string id,parent,name,space,query,cursor,limit="100",tag,root;
    bool confirm=false;
    CLI::App *rename,*move,*copy,*remove,*search,*trash,*list,*show,*restore,*purge;
    explicit State(CLI::App& app) {
        rename=app.add_subcommand("rename","Rename a node using an explicit source ETag");
        move=app.add_subcommand("mv","Move within one space; optionally rename");
        copy=app.add_subcommand("cp","Copy one file; logical quota is charged");
        remove=app.add_subcommand("rm","Move a tree to trash; quota remains charged");
        for(auto* c:{rename,move,copy,remove}){c->add_option("NODE_ID",id)->required();c->add_option("--if-match",tag)->required();}
        rename->add_option("--name",name)->required();move->add_option("--parent",parent)->required();move->add_option("--name",name);
        copy->add_option("--space",space)->required();copy->add_option("--parent",parent)->required();copy->add_option("--name",name)->required();
        search=app.add_subcommand("search","Search one page of active names literally");
        search->add_option("--space",space)->required();search->add_option("--query",query)->required();
        trash=app.add_subcommand("trash","List, restore or permanently delete trash");trash->require_subcommand(1,1);
        list=trash->add_subcommand("list","Read one page");list->add_option("--space",space)->required();list->add_option("--root-node",root);
        for(auto* c:{search,list}){c->add_option("--limit",limit);c->add_option("--cursor",cursor);}
        show=trash->add_subcommand("show","Read one entry and its ETag");
        restore=trash->add_subcommand("restore","Restore a tree; quota remains charged");
        purge=trash->add_subcommand("purge","Permanently delete; physical GC may remain pending");
        for(auto* c:{show,restore,purge})c->add_option("TRASH_ID",id)->required();
        for(auto* c:{restore,purge})c->add_option("--if-match",tag)->required();
        restore->add_option("--parent",parent);restore->add_option("--name",name);
        purge->add_flag("--confirm-permanent",confirm)->required()->disable_flag_override();
    }
    bool selected() const{return *rename||*move||*copy||*remove||*search||*trash;}
};
DriveCommands::DriveCommands(CLI::App& app):state_(std::make_unique<State>(app)){}
DriveCommands::~DriveCommands()=default;
bool DriveCommands::validate(ClientOutput& output) {
    auto& s=*state_;if(!s.selected())return true;
    output.operation=*s.rename?"rename":*s.move?"mv":*s.copy?"cp":*s.remove?"rm":*s.search?"search":*s.list?"trash list":*s.show?"trash show":*s.restore?"trash restore":"trash purge";
    if(!s.id.empty()){if(*s.trash){if(!TrashId::parse(s.id))return false;}else if(!NodeId::parse(s.id))return false;output.sourceId=s.id;}
    if((*s.rename||*s.move||*s.copy||*s.remove||*s.restore||*s.purge)&&!driveCondition(s.tag))return false;
    if((*s.copy||*s.search||*s.list)&&!SpaceId::parse(s.space))return false;
    if((*s.move||*s.copy||s.restore->count("--parent"))&&!NodeId::parse(s.parent))return false;
    if((*s.rename||*s.copy||s.move->count("--name")||s.restore->count("--name"))&&!validateDirectoryName(s.name))return false;
    if(*s.search&&!SearchText::parse(s.query))return false;
    if(*s.list&&s.list->count("--root-node")&&!NodeId::parse(s.root))return false;
    auto n=parseTransferDecimal(s.limit);
    if((*s.search||*s.list)&&(!n||n.value().value()<1||n.value().value()>200||((s.search->count("--cursor")||s.list->count("--cursor"))&&(s.cursor.empty()||s.cursor.size()>2048))))return false;
    return !*s.purge||s.confirm;
}
std::optional<int> DriveCommands::run(LockedProfile& profile,HttpTransport& transport,const std::atomic_bool* cancel,ClientOutput& output) {
    auto& s=*state_;if(!s.selected())return {};
    DriveClient api(transport,profile,cancel);
    auto emit=[&](auto reply){if(!reply)return output.error(reply.error());output.remote=RemoteOutcome::ConfirmedSuccess;output.confirmed=reply.value().meta;return output.success(clientData(reply.value().data),reply.value().meta);};
    PageRequest page{static_cast<unsigned>(parseTransferDecimal(s.limit).value().value()),s.cursor.empty()?std::nullopt:std::optional<std::string>(s.cursor)};
    if(*s.search)return emit(api.search(SpaceId::parse(s.space).value(),s.query,page));
    if(*s.list)return emit(api.listTrash(SpaceId::parse(s.space).value(),s.root.empty()?std::nullopt:std::optional<NodeId>(NodeId::parse(s.root).value()),page));
    if(*s.show)return emit(api.showTrash(TrashId::parse(s.id).value()));
    auto condition=driveCondition(s.tag).value();
    std::optional<NodeId> parent;if(!s.parent.empty())parent=NodeId::parse(s.parent).value();
    std::optional<std::string> name;if(*s.rename||*s.copy||s.move->count("--name")||s.restore->count("--name"))name=s.name;
    output.recovery="Outcome unknown: query the source/target or trash explicitly; do not automatically retry or replace the ETag. A missing entry does not prove this operation succeeded.";
    if(*s.rename||*s.move)return emit(api.update(NodeId::parse(s.id).value(),parent,name,condition));
    if(*s.copy)return emit(api.copy(NodeId::parse(s.id).value(),SpaceId::parse(s.space).value(),*parent,*name,condition));
    if(*s.remove){output.notice="Moved to trash; quota remains charged.";return emit(api.remove(NodeId::parse(s.id).value(),condition));}
    if(*s.restore){output.notice="Restored; quota remains charged.";return emit(api.restore(TrashId::parse(s.id).value(),parent,name,condition));}
    output.notice="Permanent logical deletion confirmed; physical reclamation may remain pending.";
    return emit(api.purge(TrashId::parse(s.id).value(),condition));
}
}
