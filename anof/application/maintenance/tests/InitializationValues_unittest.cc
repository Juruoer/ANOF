#define BOOST_TEST_MODULE AnofInitializationValues
#include <boost/test/unit_test.hpp>
#include <anof/application/maintenance/Initialization.h>
#include <anof/application/identity/IdentityCommands.h>
#include <anof/application/ports/MetadataReadSnapshot.h>
#include <anof/application/ports/InstanceInitialization.h>
#include <type_traits>

using namespace anof;
static_assert(!std::is_copy_constructible_v<BootstrapSeed>);
static_assert(!std::is_copy_constructible_v<RegisterCommand>);
static_assert(!std::is_copy_constructible_v<LoginResult>);
static_assert(std::is_abstract_v<MetadataReadSnapshot>);
static_assert(std::is_abstract_v<InstanceInitializer>);
static_assert(!std::is_base_of_v<UserRepository, UserReader>);

BOOST_AUTO_TEST_CASE(root_nodes_must_fit_all_three_quotas) {
    const QuotaDelta root{ByteCount{}, NodeCount::from(1).value()};
    InitialQuotas quotas{root, root, root};
    BOOST_CHECK(validateInitialQuotas(quotas));
    quotas.contribution.nodes = NodeCount{};
    BOOST_CHECK(!validateInitialQuotas(quotas));
    quotas = {root, root, root};
    quotas.publicSpace.nodes = NodeCount{};
    BOOST_CHECK(!validateInitialQuotas(quotas));
    quotas = {root, root, root};
    quotas.privateSpace.nodes = NodeCount{};
    BOOST_CHECK(!validateInitialQuotas(quotas));
}
BOOST_AUTO_TEST_CASE(initialization_distinguishes_uncertain_outcomes) {
    InitResult<void> success;
    BOOST_CHECK(success);
    InitResult<void> database{InitFailure{InitError::CommitOutcomeUnknown, InitStage::Bootstrap}};
    InitResult<void> filesystem{InitFailure{InitError::PublicationOutcomeUnknown, InitStage::PublishFormat}};
    BOOST_CHECK(!database);
    BOOST_CHECK(!filesystem);
    BOOST_CHECK(database.error().code == InitError::CommitOutcomeUnknown);
    BOOST_CHECK(filesystem.error().stage == InitStage::PublishFormat);
    BOOST_CHECK_THROW(database.value(), std::bad_variant_access);
    BOOST_CHECK_THROW(success.error(), std::bad_variant_access);
}

BOOST_AUTO_TEST_CASE(bootstrap_rejects_colliding_roots_and_moved_secret) {
    const std::string first = "00000000-0000-4000-8000-000000000001";
    const std::string second = "00000000-0000-4000-8000-000000000002";
    const QuotaDelta root{ByteCount{}, NodeCount::from(1).value()};
    auto hash = EncodedPasswordHash::parse("$argon2id$v=19$m=65536,t=2,p=1$" +
        std::string(22, 'A') + "$" + std::string(43, 'A'), {65536, 3, 1}).value();
    BootstrapSeed seed{
        {InstanceId::parse(first).value(), OperationId::parse(first).value()},
        UserId::parse(first).value(), LoginName::parse("admin").value(), DisplayName::parse("管理员").value(),
        std::move(hash), SpaceId::parse(first).value(), SpaceId::parse(second).value(),
        NodeId::parse(first).value(), NodeId::parse(second).value(), {root, root, root},
        UserLimit::from(10).value(), RequestId::parse(first).value(), UtcTime{}};
    BOOST_REQUIRE(validateBootstrapSeed(seed));
    seed.privateSpaceId = seed.publicSpaceId;
    BOOST_CHECK(!validateBootstrapSeed(seed));
    seed.privateSpaceId = SpaceId::parse(second).value();
    seed.privateRootId = seed.publicRootId;
    BOOST_CHECK(!validateBootstrapSeed(seed));
    seed.privateRootId = NodeId::parse(second).value();
    auto owned = std::move(seed.encodedPassword);
    BOOST_CHECK(!validateBootstrapSeed(seed));
    BOOST_CHECK(!owned.view().empty());
}
