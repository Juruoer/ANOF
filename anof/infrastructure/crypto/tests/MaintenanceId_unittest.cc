#define BOOST_TEST_MODULE MaintenanceId
#include <anof/infrastructure/crypto/SodiumTransferIdGenerator.h>

#include <boost/test/unit_test.hpp>
#include <set>
using namespace anof;
BOOST_AUTO_TEST_CASE(maintenance_ids_are_valid_distinct_random_uuids) {
    auto factory = infrastructure::crypto::makeSodiumMaintenanceIdGenerator();
    BOOST_REQUIRE(factory);
    std::set<std::string> seen;
    auto check = [&](const auto& value) {
        BOOST_REQUIRE(value);
        const auto text = value.value().toString();
        BOOST_CHECK_EQUAL(text[14], '4');
        BOOST_CHECK(std::string("89ab").find(text[19]) != std::string::npos);
        BOOST_CHECK(seen.insert(text).second);
    };
    for (int i = 0; i < 32; ++i) {
        check(factory.value()->nextRun());
        check(factory.value()->nextPlan());
        check(factory.value()->nextAction());
        check(factory.value()->nextQuarantine());
        check(factory.value()->nextBackup());
    }
}
