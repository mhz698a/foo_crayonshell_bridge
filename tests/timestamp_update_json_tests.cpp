#include "../src/timestamp_update_json.h"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

void test_supported_fields() {
    craybridge::file_time::TimestampUpdate update{};
    craybridge::api::TimestampUpdateJsonError error{};

    const bool parsed = craybridge::api::parse_timestamp_update_json(
        R"({"creationTime":1700000000000,"lastWriteTime":1700000001000})",
        update,
        error);

    expect(parsed, "supported timestamp fields must parse");
    expect(update.creation_time == 1700000000000ULL,
           "creationTime must be retained");
    expect(!update.last_access_time.has_value(),
           "unsupplied lastAccessTime must remain unset");
    expect(update.last_write_time == 1700000001000ULL,
           "lastWriteTime must be retained");
}

void test_whitespace_and_order() {
    craybridge::file_time::TimestampUpdate update{};
    craybridge::api::TimestampUpdateJsonError error{};

    const bool parsed = craybridge::api::parse_timestamp_update_json(
        " \r\n { \"lastAccessTime\" : 1700000002000 , "
        "\"creationTime\":1700000000000 } \t",
        update,
        error);

    expect(parsed, "JSON whitespace and field order must be accepted");
    expect(update.creation_time.has_value() &&
               update.last_access_time.has_value(),
           "both supported fields must be parsed");
}

void test_rejects_empty_or_malformed_json() {
    const std::string_view invalid[] = {
        "",
        "{}",
        "[]",
        "{\"creationTime\":}",
        "{\"creationTime\":1,}",
        "{\"creationTime\":1} trailing",
        "{\"creationTime\":01}",
        "{\"creationTime\":-1}",
        "{\"creationTime\":1.0}",
        "{\"creationTime\":1e3}",
        "{\"creationTime\":\"1\"}",
        "{\"creationTime\":null}",
        "{\"creationTime\":true}"
    };

    for (const auto json : invalid) {
        craybridge::file_time::TimestampUpdate update{};
        craybridge::api::TimestampUpdateJsonError error{};
        expect(!craybridge::api::parse_timestamp_update_json(
                   json, update, error),
               "malformed or empty timestamp JSON must be rejected");
    }
}

void test_rejects_unknown_duplicate_and_unsupported_fields() {
    using craybridge::api::TimestampUpdateJsonError;

    craybridge::file_time::TimestampUpdate update{};
    TimestampUpdateJsonError error{};

    expect(!craybridge::api::parse_timestamp_update_json(
               R"({"path":"C:\\music\\song.flac","creationTime":1})",
               update, error) &&
               error == TimestampUpdateJsonError::UnknownField,
           "arbitrary path fields must be rejected");

    expect(!craybridge::api::parse_timestamp_update_json(
               R"({"creationTime":1,"creationTime":2})",
               update, error) &&
               error == TimestampUpdateJsonError::DuplicateField,
           "duplicate timestamp fields must be rejected");

    expect(!craybridge::api::parse_timestamp_update_json(
               R"({"changeTime":1})",
               update, error) &&
               error == TimestampUpdateJsonError::UnsupportedField,
           "Windows ChangeTime writes must be rejected explicitly");
}

void test_rejects_timestamp_overflow() {
    craybridge::file_time::TimestampUpdate update{};
    craybridge::api::TimestampUpdateJsonError error{};

    expect(!craybridge::api::parse_timestamp_update_json(
               R"({"creationTime":18446744073709551615})",
               update, error) &&
               error == craybridge::api::TimestampUpdateJsonError::InvalidTimestamp,
           "FILETIME range overflow must be rejected before scheduling");
}

} // namespace

int run_timestamp_update_json_tests() {
    test_supported_fields();
    test_whitespace_and_order();
    test_rejects_empty_or_malformed_json();
    test_rejects_unknown_duplicate_and_unsupported_fields();
    test_rejects_timestamp_overflow();

    if (g_failures != 0) {
        std::cerr << g_failures << " timestamp JSON test(s) failed.\n";
        return 1;
    }

    std::cout << "All timestamp JSON tests passed.\n";
    return 0;
}
