#define BOOST_TEST_MODULE AnofDrivePolicy
#include <boost/test/unit_test.hpp>
#include <anof/application/drive/DriveCommands.h>
#include <anof/domain/drive/Blob.h>
#include <anof/domain/transfer/Storage.h>
#include <anof/domain/transfer/TransferPolicy.h>
#include <type_traits>
using namespace anof;
namespace {
template<class T> T id(int n=1) {
    auto text = std::string("10000000-0000-4000-8000-000000000000");
    const char* hex = "0123456789abcdef";
    for (int i=35; n && i>=24; --i, n/=16) text[static_cast<std::size_t>(i)] = hex[n%16];
    return T::parse(text).value();
}
NodeCount count(std::int64_t n) { return NodeCount::from(n).value(); }
ByteCount bytes(std::int64_t n) { return ByteCount::from(n).value(); }
UtcTime time(std::int64_t n) { return UtcTime{std::chrono::microseconds{n}}; }
TreeMember directory(int n, std::optional<NodeId> parent, int user=1) {
    return {{{id<NodeId>(n),id<SpaceId>(),parent,"name"+std::to_string(n),id<UserId>(user),{},time(1),time(2)},DirectoryContent{}},{}};
}
TreeSnapshot tree() { return {id<NodeId>(2), {directory(2,id<NodeId>(1)),directory(3,id<NodeId>(2))}}; }
}
static_assert(!std::is_convertible_v<TrashId,NodeId>);
BOOST_AUTO_TEST_CASE(node_capacity_transitions_are_atomic_checked_and_zero_is_noop) {
    NodeAccounting n{count(99999),{}, {}};
    auto reserved=reserveNodes(n,count(1));BOOST_REQUIRE(reserved);
    auto denied=chargeNodes(reserved.value(),count(1));BOOST_REQUIRE(!denied);
    BOOST_CHECK(denied.error().code==ErrorCode::NodeLimitReached);
    auto published=publishNodes(reserved.value(),count(1));BOOST_REQUIRE(published);
    BOOST_CHECK_EQUAL(published.value().used.value(),100000);
    BOOST_CHECK_EQUAL(published.value().reserved.value(),0);
    BOOST_CHECK(!publishNodes(published.value(),count(1)));
    BOOST_CHECK(!releaseUsedNodes(n,count(100000)));
    auto same=chargeNodes(n,count(0));BOOST_REQUIRE(same);BOOST_CHECK(same.value().revision==n.revision);
    n.revision=Revision::from(INT64_MAX).value();BOOST_CHECK(!chargeNodes(n,count(1)));
    BOOST_CHECK(!releaseReservedNodes({count(100001),{},{}},count(0)));
}
BOOST_AUTO_TEST_CASE(search_is_literal_utf8_bounded_and_cursor_bound) {
    BOOST_CHECK(validateQueryBudget(QueryBudget{}));
    QueryBudget unlimited;unlimited.elapsed=std::chrono::milliseconds{0};BOOST_CHECK(!validateQueryBudget(unlimited));
    for (auto text : {"%_\\/", ".", "..", " ", "中文"}) BOOST_CHECK(SearchText::parse(text));
    for (auto text : {std::string{},std::string(129,'x'),std::string("a\0b",3),std::string("\xc0\x80",2),std::string("\xc2\x80",2)})
        BOOST_CHECK(!SearchText::parse(text));
    auto q=SearchText::parse("%_").value();
    SearchNodesCommand c{id<SpaceId>(),q,{},SearchCursor{id<UserId>(),id<SpaceId>(),q,{"a",id<NodeId>()}}};
    BOOST_CHECK(validateDriveCommand(id<UserId>(),c));
    BOOST_CHECK(!validateDriveCommand(id<UserId>(2),c));
    c.text=SearchText::parse("other").value();BOOST_CHECK(!validateDriveCommand(id<UserId>(),c));
    BOOST_CHECK(!validateDriveCommand(UpdateNodeCommand{id<NodeId>(),NodeCondition{id<NodeId>(),{}},{},{}}));
    BOOST_CHECK(validateDriveCommand(RestoreTrashCommand{id<TrashId>(),TrashCondition{id<TrashId>(),{}},{},{}}));
}
BOOST_AUTO_TEST_CASE(tree_integrity_permissions_destination_and_limits) {
    auto t=tree();auto summary=summarizeTree(t);BOOST_REQUIRE(summary);
    BOOST_CHECK_EQUAL(summary.value().height,1);BOOST_CHECK_EQUAL(summary.value().total.nodes.value(),2);
    Space space{id<SpaceId>(),SpaceKind::Public,{},id<NodeId>(1)};
    Principal member{id<UserId>(),UserRole::Member,{}};
    BOOST_CHECK(requireTreeMutation(member,space,t));
    t.members[1].node.metadata.createdBy=id<UserId>(2);
    BOOST_CHECK(!requireTreeMutation(member,space,t));
    member.role=UserRole::Administrator;BOOST_CHECK(requireTreeMutation(member,space,t));
    DirectoryPath path{directory(1,{}).node.metadata};BOOST_CHECK(validateTreeDestination(t,path));
    path.push_back(t.members[0].node.metadata);
    auto cycle=validateTreeDestination(t,path);BOOST_REQUIRE(!cycle);BOOST_CHECK(cycle.error().code==ErrorCode::DirectoryCycle);
    t.members[1].node.metadata.parentId=id<NodeId>(99);BOOST_CHECK(!summarizeTree(t));
    t=tree();t.members.push_back(t.members[1]);BOOST_CHECK(!summarizeTree(t));
    t={id<NodeId>(2),{directory(2,id<NodeId>(1))}};
    for (int i=3;i<=1001;++i)t.members.push_back(directory(i,id<NodeId>(2)));
    BOOST_REQUIRE(summarizeTree(t));t.members.push_back(directory(1002,id<NodeId>(2)));
    auto tooBig=summarizeTree(t);BOOST_REQUIRE(!tooBig);BOOST_CHECK(tooBig.error().code==ErrorCode::TreeLimitExceeded);
}
BOOST_AUTO_TEST_CASE(retention_and_gc_require_ordered_evidence_and_no_references) {
    TrashEntry e{id<TrashId>(),id<SpaceId>(),id<NodeId>(2),id<NodeId>(1),"x",NodeKind::File,id<UserId>(),
        time(10),trashExpiry(time(10)).value(),{},count(1),bytes(3),id<UserId>()};
    BOOST_REQUIRE(validateTrashEntry(e));BOOST_CHECK(!trashExpired(e,time(9)));BOOST_CHECK(trashExpired(e,e.expiresAt));
    BOOST_CHECK(!trashExpiry(time(INT64_MAX)));
    BlobRecord b{{id<BlobId>(),FileLength::from(3).value(),Sha256::parse(std::string(64,'a')).value()},
        BlobState::Available,time(1),time(10),{},time(10),{}};
    auto due=blobGcDeadline(time(10)).value();
    BOOST_CHECK(blobGcEligible(b,due,false,false));BOOST_CHECK(!blobGcEligible(b,due,true,false));
    BOOST_CHECK(!blobGcEligible(b,due,false,true));BOOST_CHECK(!blobGcEligible(b,time(9),false,false));
    b.state=BlobState::Deleting;BOOST_CHECK(!validateBlobRecord(b));
    b.deleteStartedAt=time(10);BOOST_CHECK(!validateBlobRecord(b));
    b.deleteStartedAt=due;b.updatedAt=due;BOOST_REQUIRE(validateBlobRecord(b));BOOST_CHECK(!blobGcEligible(b,due,false,false));
    BOOST_CHECK(!blobGcDeadline(time(INT64_MAX)));
}
BOOST_AUTO_TEST_CASE(logical_and_physical_release_are_distinct) {
    QuotaState q{{bytes(10),count(10)},{bytes(3),count(2)},{bytes(2),count(1)}};
    auto released=releaseUsedQuota(q,{bytes(3),count(2)});BOOST_REQUIRE(released);
    BOOST_CHECK_EQUAL(released.value().reserved.bytes.value(),2);BOOST_CHECK(!releaseUsedQuota(q,{bytes(4),{}}));
    StorageSnapshot storage{bytes(10),bytes(3),bytes(2),{}};
    auto gc=releaseRetainedStorage(storage,bytes(3));BOOST_REQUIRE(gc);
    BOOST_CHECK_EQUAL(gc.value().retained.value(),0);BOOST_CHECK_EQUAL(gc.value().reserved.value(),2);
    BOOST_CHECK(!releaseRetainedStorage(gc.value(),bytes(3)));
}
BOOST_AUTO_TEST_CASE(completed_upload_deletion_evidence_is_irreversible) {
    UploadTarget target{id<NodeId>(),"f"};
    Upload u{id<UploadId>(),id<UserId>(),id<OperationId>(),{id<SpaceId>(),target,FileLength::from(0).value(),{}},target,
        {},UploadState::Completed,{},Sha256::parse(std::string(64,'a')).value(),id<BlobId>(),StagingLocation::Object,
        id<NodeId>(2),time(1),time(1),uploadExpiry(time(1),time(1)).value(),time(2),{}};
    BOOST_REQUIRE(validateUpload(u));
    auto after=u;after.generation=u.generation.add(1).value();after.resultDeletedAt=time(2);
    BOOST_REQUIRE(validateUploadReplacement(u,after));
    auto removed=after;removed.generation=after.generation.add(1).value();removed.resultDeletedAt.reset();
    BOOST_CHECK(!validateUploadReplacement(after,removed));
    after.resultDeletedAt=time(1);BOOST_CHECK(!validateUpload(after));
}

BOOST_AUTO_TEST_CASE(destination_depth_counts_subtree_and_shared_content_counts_each_node) {
    TreeSnapshot t{id<NodeId>(500),{directory(500,id<NodeId>(1))}};
    DirectoryPath path;
    for (int i=1;i<=128;++i) path.push_back(directory(i,i==1?std::optional<NodeId>{}:id<NodeId>(i-1)).node.metadata);
    BOOST_REQUIRE(validateTreeDestination(t,path));
    t.members.push_back(directory(501,id<NodeId>(500)));
    auto deep=validateTreeDestination(t,path);BOOST_REQUIRE(!deep);BOOST_CHECK(deep.error().code==ErrorCode::DepthLimitExceeded);
    path.pop_back();BOOST_REQUIRE(validateTreeDestination(t,path));
    auto a=directory(502,id<NodeId>(500));
    a.node.content=FileContentMetadata{bytes(7),{}};a.blobId=id<BlobId>();
    auto b=a;b.node.metadata.id=id<NodeId>(503);b.node.metadata.createdBy=id<UserId>(2);
    t.members.push_back(a);t.members.push_back(b);
    auto sum=summarizeTree(t);BOOST_REQUIRE(sum);
    BOOST_CHECK_EQUAL(sum.value().total.bytes.value(),14);
    BOOST_CHECK_EQUAL(sum.value().byCreator.at(id<UserId>(2)).bytes.value(),7);
    BOOST_CHECK(!sum.value().soleCreator);
}
