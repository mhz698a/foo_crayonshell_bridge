#include "../src/file_creation_time.h"

#include <Windows.h>

#include <cstdint>
#include <iostream>
#include <limits>

namespace {

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

bool within_tolerance(std::uint64_t actual,
                      std::uint64_t expected,
                      std::uint64_t tolerance) {
    return actual > expected
        ? actual - expected <= tolerance
        : expected - actual <= tolerance;
}

bool create_temp_file(wchar_t (&tempFile)[MAX_PATH]) {
    wchar_t tempPath[MAX_PATH]{};
    const DWORD pathLength = ::GetTempPathW(MAX_PATH, tempPath);
    if (pathLength == 0 || pathLength >= MAX_PATH) {
        expect(false, "GetTempPathW must succeed");
        return false;
    }

    if (::GetTempFileNameW(tempPath, L"csb", 0, tempFile) == 0) {
        expect(false, "GetTempFileNameW must create a temporary file");
        return false;
    }
    return true;
}

void test_epoch_conversion() {
    using craybridge::file_time::filetime_ticks_to_unix_milliseconds;
    using craybridge::file_time::unix_milliseconds_to_filetime_ticks;
    using craybridge::file_time::kWindowsToUnixEpoch100ns;

    const auto epoch = filetime_ticks_to_unix_milliseconds(kWindowsToUnixEpoch100ns);
    expect(epoch.has_value() && *epoch == 0, "Unix epoch must convert to 0 ms");

    const auto oneMillisecond =
        filetime_ticks_to_unix_milliseconds(kWindowsToUnixEpoch100ns + 10000ULL);
    expect(oneMillisecond.has_value() && *oneMillisecond == 1,
           "one millisecond after epoch must convert to 1 ms");

    const auto oneSecond =
        filetime_ticks_to_unix_milliseconds(kWindowsToUnixEpoch100ns + 10000000ULL);
    expect(oneSecond.has_value() && *oneSecond == 1000,
           "one second after epoch must convert to 1000 ms");

    const auto beforeEpoch =
        filetime_ticks_to_unix_milliseconds(kWindowsToUnixEpoch100ns - 1ULL);
    expect(!beforeEpoch.has_value(), "pre-epoch FILETIME must be rejected");

    const auto unixEpochTicks = unix_milliseconds_to_filetime_ticks(0);
    expect(unixEpochTicks.has_value() &&
               *unixEpochTicks == kWindowsToUnixEpoch100ns,
           "Unix epoch must convert back to FILETIME");

    const auto oneMillisecondTicks = unix_milliseconds_to_filetime_ticks(1);
    expect(oneMillisecondTicks.has_value() &&
               *oneMillisecondTicks == kWindowsToUnixEpoch100ns + 10000ULL,
           "one millisecond must convert back to FILETIME");

    const auto overflow =
        unix_milliseconds_to_filetime_ticks((std::numeric_limits<std::uint64_t>::max)());
    expect(!overflow.has_value(), "overflowing Unix milliseconds must be rejected");
}

void test_empty_path() {
    const auto result = craybridge::file_time::query_file_timestamps(L"");
    expect(!result.success, "empty path timestamp query must fail");
    expect(result.win32_error == ERROR_INVALID_PARAMETER,
           "empty path must report ERROR_INVALID_PARAMETER");

    const auto legacy =
        craybridge::file_time::query_creation_time_unix_milliseconds(L"");
    expect(!legacy.success, "legacy empty-path query must fail");
}

void test_existing_file_timestamps() {
    wchar_t tempFile[MAX_PATH]{};
    if (!create_temp_file(tempFile)) return;

    const auto result = craybridge::file_time::query_file_timestamps(tempFile);
    expect(result.success, "existing file timestamps must be readable");
    expect(result.win32_error == ERROR_SUCCESS,
           "successful timestamp query must report ERROR_SUCCESS");
    expect(result.timestamps.creation_time.has_value(),
           "creation time must be present");
    expect(result.timestamps.last_access_time.has_value(),
           "last access time must be present");
    expect(result.timestamps.last_write_time.has_value(),
           "last write time must be present");
    expect(result.timestamps.change_time.has_value(),
           "Windows change time must be readable");

    const auto legacy =
        craybridge::file_time::query_creation_time_unix_milliseconds(tempFile);
    expect(legacy.success, "legacy creation-time query must remain functional");
    expect(legacy.unix_milliseconds > 0,
           "temporary file creation timestamp must be positive");

    ::DeleteFileW(tempFile);
}

void test_write_supported_timestamps() {
    wchar_t tempFile[MAX_PATH]{};
    if (!create_temp_file(tempFile)) return;

    constexpr std::uint64_t creation = 1700000000000ULL;
    constexpr std::uint64_t access = 1700000001000ULL;
    constexpr std::uint64_t write = 1700000002000ULL;

    craybridge::file_time::TimestampUpdate update{};
    update.creation_time = creation;
    update.last_access_time = access;
    update.last_write_time = write;

    const auto result =
        craybridge::file_time::write_file_timestamps(tempFile, update);
    expect(result.success, "SetFileTime must update supported timestamps");
    expect(result.win32_error == ERROR_SUCCESS,
           "successful timestamp write must report ERROR_SUCCESS");
    expect(result.readback_success,
           "timestamp values must be readable after a successful write");
    expect(result.readback_error == ERROR_SUCCESS,
           "successful timestamp readback must report ERROR_SUCCESS");

    if (result.readback_success) {
        constexpr std::uint64_t tolerance = 2000;
        expect(result.timestamps.creation_time.has_value() &&
                   within_tolerance(*result.timestamps.creation_time,
                                    creation, tolerance),
               "creation time must match the requested value");
        expect(result.timestamps.last_access_time.has_value() &&
                   within_tolerance(*result.timestamps.last_access_time,
                                    access, tolerance),
               "last access time must match the requested value");
        expect(result.timestamps.last_write_time.has_value() &&
                   within_tolerance(*result.timestamps.last_write_time,
                                    write, tolerance),
               "last write time must match the requested value");
    }

    ::DeleteFileW(tempFile);
}

void test_change_time_is_read_only() {
    wchar_t tempFile[MAX_PATH]{};
    if (!create_temp_file(tempFile)) return;

    craybridge::file_time::TimestampUpdate update{};
    update.change_time = 1700000000000ULL;

    const auto result =
        craybridge::file_time::write_file_timestamps(tempFile, update);
    expect(!result.success, "ChangeTime write must be rejected");
    expect(result.win32_error == ERROR_NOT_SUPPORTED,
           "ChangeTime write must report ERROR_NOT_SUPPORTED");

    ::DeleteFileW(tempFile);
}

void test_missing_file() {
    wchar_t tempFile[MAX_PATH]{};
    if (!create_temp_file(tempFile)) return;

    ::DeleteFileW(tempFile);
    const auto result = craybridge::file_time::query_file_timestamps(tempFile);
    expect(!result.success, "missing file must fail");
    expect(result.win32_error == ERROR_FILE_NOT_FOUND ||
               result.win32_error == ERROR_PATH_NOT_FOUND,
           "missing file must report FILE_NOT_FOUND or PATH_NOT_FOUND");
}

void test_directory_is_rejected() {
    wchar_t tempPath[MAX_PATH]{};
    const DWORD pathLength = ::GetTempPathW(MAX_PATH, tempPath);
    if (pathLength == 0 || pathLength >= MAX_PATH) {
        expect(false, "GetTempPathW must succeed for directory test");
        return;
    }

    const auto result = craybridge::file_time::query_file_timestamps(tempPath);
    expect(!result.success, "directory must be rejected");
    expect(result.win32_error == ERROR_DIRECTORY,
           "directory must report ERROR_DIRECTORY");
}

} // namespace

int wmain() {
    test_epoch_conversion();
    test_empty_path();
    test_existing_file_timestamps();
    test_write_supported_timestamps();
    test_change_time_is_read_only();
    test_missing_file();
    test_directory_is_rejected();

    if (g_failures != 0) {
        std::cerr << g_failures << " test(s) failed.\n";
        return 1;
    }

    std::cout << "All file timestamp tests passed.\n";
    return 0;
}
