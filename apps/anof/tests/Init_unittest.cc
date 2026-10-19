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
    args.contentBudget = "1099511627776";
    auto valid = parseInitOptions(args);
    BOOST_REQUIRE(valid);
    BOOST_CHECK_EQUAL(valid.value().instance.adminLogin.value(), "admin");
    BOOST_CHECK_EQUAL(valid.value().instance.quotas.privateSpace.bytes.value(), 1LL << 40);
    for (const auto* text : {"", "-1", "+1", "01", "1.0", "1e3", "9223372036854775808"}) {
        auto bad = args; bad.quotas[0] = text;
        BOOST_CHECK(!parseInitOptions(bad));
    }
    auto bad = args; bad.quotas[1] = "0"; BOOST_CHECK(!parseInitOptions(bad));
    bad = args; bad.developmentHttp = false; BOOST_CHECK(!parseInitOptions(bad));
    bad = args; bad.maxUsers = "10001"; BOOST_CHECK(!parseInitOptions(bad));
    bad = args; bad.root = "relative"; BOOST_CHECK(!parseInitOptions(bad));
}

BOOST_AUTO_TEST_CASE(transfer_budget_and_safety_are_explicit_checked_values) {
    InitArguments args;
    args.root = "/test/instance"; args.login = "admin"; args.displayName = "Admin";
    args.quotas = {"0", "1", "0", "1", "0", "1"};
    args.developmentHttp = true;
    BOOST_CHECK(!parseInitOptions(args));
    for (const auto* text : {"", "-1", "+1", "01", " 1", "1e3", "9223372036854775808"}) {
        args.contentBudget = text;
        BOOST_CHECK(!parseInitOptions(args));
    }
    args.contentBudget = "9223372036854775807";
    auto full = parseInitOptions(args);
    BOOST_REQUIRE(full);
    BOOST_CHECK_EQUAL(full.value().contentBudget.value(), INT64_MAX);
    BOOST_CHECK_EQUAL(full.value().instance.configuration.diskSafety.minimum.value(), 1073741824);
    BOOST_CHECK_EQUAL(full.value().instance.configuration.diskSafety.percent.value(), 5);
    args.contentBudget = "0"; args.diskSafetyMinimum = "0"; args.diskSafetyPercent = "100";
    auto zero = parseInitOptions(args);
    BOOST_REQUIRE(zero);
    BOOST_CHECK_EQUAL(zero.value().contentBudget.value(), 0);
    BOOST_CHECK_EQUAL(zero.value().instance.configuration.diskSafety.minimum.value(), 0);
    BOOST_CHECK_EQUAL(zero.value().instance.configuration.diskSafety.percent.value(), 100);
    for (const auto* text : {"101", "-1", "01", ""}) {
        args.diskSafetyPercent = text;
        BOOST_CHECK(!parseInitOptions(args));
    }
    args.diskSafetyPercent = "5"; args.diskSafetyMinimum = "9223372036854775808";
    BOOST_CHECK(!parseInitOptions(args));
}
