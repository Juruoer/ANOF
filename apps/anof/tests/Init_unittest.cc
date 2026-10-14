#define BOOST_TEST_MODULE InitCommand
#include <boost/test/unit_test.hpp>
#include <apps/anof/Init.h>
using namespace anof;
using namespace anof::app;
BOOST_AUTO_TEST_CASE(explicit_quota_input_is_checked_before_password_or_disk) {
    InitArguments args;
    args.root = "/test/instance"; args.login = "Admin"; args.displayName = "家庭";
    args.quotas = {"1099511627776", "1", "0", "1", "0", "1"};
    args.developmentHttp = true;
    auto valid = parseInitOptions(args);
    BOOST_REQUIRE(valid);
    BOOST_CHECK_EQUAL(valid.value().adminLogin.value(), "admin");
    BOOST_CHECK_EQUAL(valid.value().quotas.privateSpace.bytes.value(), 1LL << 40);
    for (const auto* text : {"", "-1", "+1", "01", "1.0", "1e3", "9223372036854775808"}) {
        auto bad = args; bad.quotas[0] = text;
        BOOST_CHECK(!parseInitOptions(bad));
    }
    auto bad = args; bad.quotas[1] = "0"; BOOST_CHECK(!parseInitOptions(bad));
    bad = args; bad.developmentHttp = false; BOOST_CHECK(!parseInitOptions(bad));
    bad = args; bad.maxUsers = "10001"; BOOST_CHECK(!parseInitOptions(bad));
    bad = args; bad.root = "relative"; BOOST_CHECK(!parseInitOptions(bad));
}
