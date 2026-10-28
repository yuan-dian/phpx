#include "phpx_test.h"

using namespace php;

TEST(enum_case, valid_cases_preserve_identity) {
    eval("enum PhpxCheckedUnit { case Value; } enum PhpxCheckedBacked: int { case Value = 42; }");
    auto *unit = getClassEntrySafe("PhpxCheckedUnit");
    auto first = getEnumCase(unit, "Value");
    auto second = getEnumCase(unit, "Value");
    ASSERT_EQ(first.object(), second.object());
    ASSERT_STREQ(getEnumCaseName(first).data(), "Value");

    auto backed = getEnumCase(getClassEntrySafe("PhpxCheckedBacked"), "Value");
    ASSERT_EQ(backed.get("value").toInt(), 42);
    ASSERT_STREQ(getEnumCaseName(backed).data(), "Value");

    auto internal = getEnumCase(getClassEntrySafe("RoundingMode"), "HalfEven");
    ASSERT_STREQ(getEnumCaseName(internal).data(), "HalfEven");
}

TEST(enum_case, missing_and_wrong_case_names_throw) {
    eval("enum PhpxCheckedMissing { case Value; }");
    auto *ce = getClassEntrySafe("PhpxCheckedMissing");
    try_call([ce]() { getEnumCase(ce, "Missing"); }, "Undefined constant PhpxCheckedMissing::Missing");
    try_call([ce]() { getEnumCase(ce, "value"); }, "Undefined constant PhpxCheckedMissing::value");
}

TEST(enum_case, null_and_non_enum_class_entries_throw) {
    try_call([]() { getEnumCase(nullptr, "Missing"); }, "Cannot fetch enum case from null class entry");
    try_call([]() { getEnumCase(getClassEntrySafe("stdClass"), "Missing"); }, "Class stdClass is not an enum");
}

TEST(enum_case, ordinary_constants_are_not_cases) {
    eval("enum PhpxCheckedConstants { case Value; public const LABEL = 'value'; public const ALIAS = self::Value; }");
    auto *ce = getClassEntrySafe("PhpxCheckedConstants");
    try_call([ce]() { getEnumCase(ce, "LABEL"); }, "Constant PhpxCheckedConstants::LABEL is not an enum case");
    try_call([ce]() { getEnumCase(ce, "ALIAS"); }, "Constant PhpxCheckedConstants::ALIAS is not an enum case");
}

TEST(enum_case, lazy_initialization_errors_propagate) {
    eval("enum PhpxCheckedFailing: int { case Value = PHPX_ENUM_UNDEFINED_BACKING; }");
    auto *ce = getClassEntrySafe("PhpxCheckedFailing");
    try_call([ce]() { getEnumCase(ce, "Value"); }, "Undefined constant \"PHPX_ENUM_UNDEFINED_BACKING\"");
}

TEST(enum_case, name_rejects_empty_and_non_enum_objects) {
    Object empty;
    auto ordinary = newObject("stdClass");
    try_call([&]() { getEnumCaseName(empty); }, "Expected an enum case object");
    try_call([&]() { getEnumCaseName(ordinary); }, "Expected an enum case object");
}
