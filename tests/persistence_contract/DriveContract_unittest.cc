#define BOOST_TEST_MODULE AnofDriveContract
#include <boost/test/unit_test.hpp>
#include <tests/persistence_contract/TestDriveBackend.h>
using namespace anof;
using namespace anof::testing;
namespace {
template<class T> T id() { return T::parse("11111111-1111-4111-8111-111111111111").value(); }
ByteCount bytes(std::int64_t n) { return ByteCount::from(n).value(); }
NodeCount nodes(std::int64_t n) { return NodeCount::from(n).value(); }
UtcTime now{std::chrono::microseconds{1000000}};
DriveSeed seed() {
    MetadataSeed m;
    m.users.push_back({{id<UserId>(),UserRole::Administrator,{}},true});
    m.spaces.push_back({id<SpaceId>(),SpaceKind::Public,{},id<NodeId>()});
    m.nodes.push_back({{id<NodeId>(),id<SpaceId>(),{},"",id<UserId>(),{},now,now},{},false});
    m.quotas.push_back({SpaceQuota{id<SpaceId>()},{{bytes(100),nodes(100)},{bytes(0),nodes(1)},{}}});
    return {{m,{bytes(100),{}, {}, {}},{}},{},{},{},{nodes(1),{}, {}}};
}
}
BOOST_AUTO_TEST_CASE(drive_views_share_transaction_and_rollback_node_reservations) {
    auto backend=makeTestDriveBackend();BOOST_REQUIRE(backend);BOOST_REQUIRE(backend->reset(seed()));
    auto store=backend->connect();BOOST_REQUIRE(store);
    {
        auto tx=store.value()->beginDriveWrite();BOOST_REQUIRE(tx);
        BOOST_CHECK(!store.value()->beginTransferRead());
        BOOST_REQUIRE(tx.value()->nodeAccounting().reserve(nodes(1)));
        auto inside=tx.value()->nodeAccounting().get();BOOST_REQUIRE(inside);
        BOOST_CHECK_EQUAL(inside.value().reserved.value(),1);
        // 未提交析构必须回滚，而非自动提交。
    }
    store.value().reset();store=backend->connect();BOOST_REQUIRE(store);
    auto read=store.value()->beginDriveRead();BOOST_REQUIRE(read);
    auto persisted=read.value()->nodeAccounting().get();BOOST_REQUIRE(persisted);
    BOOST_CHECK_EQUAL(persisted.value().reserved.value(),0);
    BOOST_CHECK_EQUAL(persisted.value().revision.value(),1);
}
BOOST_AUTO_TEST_CASE(gc_intent_survives_reopen_and_physical_release_is_exactly_once) {
    auto s=seed();auto digest=Sha256::parse(std::string(64,'a')).value();
    BlobRecord record{{id<BlobId>(),FileLength::from(7).value(),digest},BlobState::Available,now,now,{},now,{}};
    s.blobRecords.push_back(record);
    s.transfer.metadata.blobs.push_back({id<BlobId>(),bytes(7),digest.bytes()});
    s.transfer.storage.retained=bytes(7);
    auto backend=makeTestDriveBackend();BOOST_REQUIRE(backend);BOOST_REQUIRE(backend->reset(s));
    auto store=backend->connect();BOOST_REQUIRE(store);
    {
        auto tx=store.value()->beginDriveWrite();BOOST_REQUIRE(tx);
        auto early=tx.value()->blobs().markDeleting(id<BlobId>(),{},now);BOOST_CHECK(!early);
    }
    Revision deletingRevision;
    {
        auto tx=store.value()->beginDriveWrite();BOOST_REQUIRE(tx);
        auto marked=tx.value()->blobs().markDeleting(id<BlobId>(),{},blobGcDeadline(now).value());BOOST_REQUIRE(marked);
        deletingRevision=marked.value().revision;
        BOOST_REQUIRE(tx.value()->commit());
    }
    store.value().reset();store=backend->connect();BOOST_REQUIRE(store);
    {
        auto tx=store.value()->beginDriveWrite();BOOST_REQUIRE(tx);
        auto current=tx.value()->blobs().record(id<BlobId>());BOOST_REQUIRE(current&&current.value());
        BOOST_CHECK(current.value()->state==BlobState::Deleting);
        auto held=tx.value()->storage().get();BOOST_REQUIRE(held);BOOST_CHECK_EQUAL(held.value().retained.value(),7);
        BOOST_REQUIRE(tx.value()->blobs().eraseDeleting(id<BlobId>(),deletingRevision));
        BOOST_REQUIRE(tx.value()->storage().releaseRetained(bytes(7)));
        BOOST_REQUIRE(tx.value()->commit());
    }
    {
        auto tx=store.value()->beginDriveWrite();BOOST_REQUIRE(tx);
        BOOST_CHECK(!tx.value()->blobs().eraseDeleting(id<BlobId>(),deletingRevision));
    }
    auto read=store.value()->beginDriveRead();BOOST_REQUIRE(read);
    auto row=read.value()->blobLifecycle().record(id<BlobId>());BOOST_REQUIRE(row);BOOST_CHECK(!row.value());
    auto storage=read.value()->storage().get();BOOST_REQUIRE(storage);BOOST_CHECK_EQUAL(storage.value().retained.value(),0);
}
BOOST_AUTO_TEST_CASE(trash_visibility_restore_conditions_and_transaction_rollback) {
    auto s=seed();auto fileId=NodeId::parse("22222222-2222-4222-8222-222222222222").value();
    auto owner=id<UserId>();
    auto other=UserId::parse("33333333-3333-4333-8333-333333333333").value();
    s.transfer.metadata.users.push_back({{other,UserRole::Member,{}},true});
    auto digest=Sha256::parse(std::string(64,'b')).value();
    s.transfer.metadata.blobs.push_back({id<BlobId>(),bytes(7),digest.bytes()});
    s.blobRecords.push_back({{id<BlobId>(),FileLength::from(7).value(),digest},BlobState::Available,now,now,{},{},{}});
    s.transfer.metadata.nodes.push_back({{fileId,id<SpaceId>(),{},"saved",owner,{},now,now},id<BlobId>(),true});
    s.memberships.emplace_back(fileId,id<TrashId>());
    s.trashEntries.push_back({id<TrashId>(),id<SpaceId>(),fileId,id<NodeId>(),"saved",NodeKind::File,owner,
        now,trashExpiry(now).value(),{},nodes(1),bytes(7),owner});
    s.transfer.metadata.quotas[0].state.used={bytes(7),nodes(2)};
    s.transfer.storage.retained=bytes(7);s.nodeAccounting.used=nodes(2);
    auto backend=makeTestDriveBackend();BOOST_REQUIRE(backend);BOOST_REQUIRE(backend->reset(s));
    auto store=backend->connect();BOOST_REQUIRE(store);
    {
        auto tx=store.value()->beginDriveRead();BOOST_REQUIRE(tx);
        auto page=tx.value()->trash().list({{owner,UserRole::Member,{}},id<SpaceId>(),{},PageSize{}, {}});BOOST_REQUIRE(page);
        BOOST_REQUIRE_EQUAL(page.value().items.size(),1);
        auto hidden=tx.value()->trash().list({{other,UserRole::Member,{}},id<SpaceId>(),{},PageSize{}, {}});BOOST_REQUIRE(hidden);
        BOOST_CHECK(hidden.value().items.empty());
        auto search=tx.value()->tree().search({owner,UserRole::Member,{}},{id<SpaceId>(),SearchText::parse("saved").value(),{}, {}, {}});
        BOOST_REQUIRE(search);BOOST_CHECK(search.value().items.empty());
    }
    {
        auto tx=store.value()->beginDriveWrite();BOOST_REQUIRE(tx);
        auto old=tx.value()->tree().trashSubtree(id<TrashId>(),{});BOOST_REQUIRE(old);
        TreeMutationPlan plan{old.value(),{fileId,{}},id<NodeId>(),"restored",now};
        BOOST_REQUIRE(tx.value()->tree().restore(plan,{id<TrashId>(),{}}));
        BOOST_REQUIRE(tx.value()->trash().erase({id<TrashId>(),{}}));
        // 未提交恢复：树、条目和父revision必须一起回滚。
    }
    store.value().reset();store=backend->connect();BOOST_REQUIRE(store);
    {
        auto tx=store.value()->beginDriveWrite();BOOST_REQUIRE(tx);
        auto old=tx.value()->tree().trashSubtree(id<TrashId>(),{});BOOST_REQUIRE(old);
        TreeMutationPlan plan{old.value(),{fileId,{}},id<NodeId>(),"restored",now};
        auto stale=tx.value()->tree().restore(plan,{id<TrashId>(),Revision::from(2).value()});BOOST_CHECK(!stale);
    }
    auto tx=store.value()->beginDriveRead();BOOST_REQUIRE(tx);
    auto entry=tx.value()->trash().find(id<TrashId>());BOOST_REQUIRE(entry);BOOST_CHECK(bool(entry.value()));
    auto total=tx.value()->nodeAccounting().get();BOOST_REQUIRE(total);BOOST_CHECK_EQUAL(total.value().used.value(),2);
}

BOOST_AUTO_TEST_CASE(whole_tree_purge_preserves_audit_and_releases_only_logical_accounting) {
    auto s = seed();
    const auto directory = NodeId::parse("22222222-2222-4222-8222-222222222222").value();
    const auto file = NodeId::parse("33333333-3333-4333-8333-333333333333").value();
    const auto digest = Sha256::parse(std::string(64, 'c')).value();
    s.transfer.metadata.nodes.push_back({{directory, id<SpaceId>(), id<NodeId>(), "folder", id<UserId>(), {}, now, now}, {}, false});
    s.transfer.metadata.nodes.push_back({{file, id<SpaceId>(), directory, "file", id<UserId>(), {}, now, now}, id<BlobId>(), false});
    s.transfer.metadata.blobs.push_back({id<BlobId>(), bytes(7), digest.bytes()});
    s.blobRecords.push_back({{id<BlobId>(), FileLength::from(7).value(), digest}, BlobState::Available, now, now, {}, {}, {}});
    s.transfer.metadata.quotas[0].state.used = {bytes(7), nodes(3)};
    s.transfer.storage.retained = bytes(7);
    s.nodeAccounting.used = nodes(3);
    auto backend = makeTestDriveBackend();
    BOOST_REQUIRE(backend);
    BOOST_REQUIRE(backend->reset(s));
    auto store = backend->connect();
    BOOST_REQUIRE(store);
    const TrashEntry entry{id<TrashId>(), id<SpaceId>(), directory, id<NodeId>(), "folder", NodeKind::Directory,
        id<UserId>(), now, trashExpiry(now).value(), {}, nodes(2), bytes(7), id<UserId>()};
    {
        auto tx = store.value()->beginDriveWrite();
        BOOST_REQUIRE(tx);
        // 真实目录创建审计也是永久历史，不能阻止后续树删除。
        BOOST_REQUIRE(tx.value()->directoryAudit().append({id<UserId>(), id<SpaceId>(), directory, id<NodeId>(), id<RequestId>(), now}));
        auto tree = tx.value()->tree().activeSubtree(directory, {});
        BOOST_REQUIRE(tree);
        BOOST_REQUIRE(tx.value()->trash().insert(entry));
        BOOST_REQUIRE(tx.value()->tree().detach(tree.value(), {directory, {}}, entry));
        BOOST_REQUIRE(tx.value()->driveAudit().append({id<RequestId>(), now, id<UserId>(), TreeTrashed{id<TrashId>(), directory}}));
        BOOST_REQUIRE(tx.value()->commit());
    }
    store.value().reset();
    store = backend->connect();
    BOOST_REQUIRE(store);
    // 同一组合先整体回滚，再提交；父子行删除不能依赖删除顺序或级联扣账。
    for (bool commit : {false, true}) {
        auto tx = store.value()->beginDriveWrite();
        BOOST_REQUIRE(tx);
        auto tree = tx.value()->tree().trashSubtree(id<TrashId>(), {});
        BOOST_REQUIRE(tree);
        BOOST_REQUIRE_EQUAL(tree.value().members.size(), 2);
        BOOST_REQUIRE(tx.value()->tree().eraseMembers({id<TrashId>(), {}}, tree.value()));
        BOOST_REQUIRE(tx.value()->trash().erase({id<TrashId>(), {}}));
        BOOST_REQUIRE(tx.value()->quotas().releaseUsed(SpaceQuota{id<SpaceId>()}, {bytes(7), nodes(2)}));
        BOOST_REQUIRE(tx.value()->nodeAccounting().releaseUsed(nodes(2)));
        BOOST_REQUIRE(tx.value()->blobs().markUnreferenced(id<BlobId>(), {}, now));
        BOOST_REQUIRE(tx.value()->driveAudit().append({id<RequestId>(), now, id<UserId>(), TreePurged{id<TrashId>(), directory}}));
        if (commit) BOOST_REQUIRE(tx.value()->commit());
    }
    store.value().reset();
    store = backend->connect();
    BOOST_REQUIRE(store);
    auto read = store.value()->beginDriveRead();
    BOOST_REQUIRE(read);
    auto trash = read.value()->trash().find(id<TrashId>());
    BOOST_REQUIRE(trash);
    BOOST_CHECK(!trash.value());
    auto quota = read.value()->quotas().get(SpaceQuota{id<SpaceId>()});
    BOOST_REQUIRE(quota);
    BOOST_CHECK_EQUAL(quota.value().state.used.bytes.value(), 0);
    BOOST_CHECK_EQUAL(quota.value().state.used.nodes.value(), 1);
    auto ledger = read.value()->nodeAccounting().get();
    BOOST_REQUIRE(ledger);
    BOOST_CHECK_EQUAL(ledger.value().used.value(), 1);
    auto physical = read.value()->storage().get();
    BOOST_REQUIRE(physical);
    BOOST_CHECK_EQUAL(physical.value().retained.value(), 7);
    auto record = read.value()->blobLifecycle().record(id<BlobId>());
    BOOST_REQUIRE(record && record.value());
    BOOST_CHECK(record.value()->state == BlobState::Available);
    BOOST_CHECK(record.value()->unreferencedAt == now);
    read.value().reset();
    auto audit = backend->readDriveAudit();
    BOOST_REQUIRE(audit);
    BOOST_REQUIRE_EQUAL(audit.value().size(), 2);
    BOOST_CHECK(std::holds_alternative<TreeTrashed>(audit.value()[0].action));
    BOOST_CHECK(std::holds_alternative<TreePurged>(audit.value()[1].action));
}

BOOST_AUTO_TEST_CASE(new_blob_insert_rejects_preexisting_unreferenced_lifecycle) {
    auto backend = makeTestDriveBackend();
    BOOST_REQUIRE(backend);
    BOOST_REQUIRE(backend->reset(seed()));
    auto store = backend->connect();
    BOOST_REQUIRE(store);
    auto tx = store.value()->beginDriveWrite();
    BOOST_REQUIRE(tx);
    // 新发布只能创建有引用的AVAILABLE记录；无引用时间由移除最后引用后的端口记录。
    const BlobRecord record{{id<BlobId>(), FileLength::from(7).value(), Sha256::parse(std::string(64, 'd')).value()},
        BlobState::Available, now, now, {}, now, {}};
    BOOST_CHECK(!tx.value()->blobs().insert(record));
}
