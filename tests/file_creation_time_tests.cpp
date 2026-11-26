#include "../src/file_creation_time.h"

#include <Windows.h>

#include <cstdint>
#include <iostream>

namespace {

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

void test_epoch_conversion() {
    using craybridge::file_time::filetime_ticks_to_unix_milliseconds;
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
}

void test_empty_path() {
    const auto result =
        craybridge::file_time::query_creation_time_unix_milliseconds(L"");
    expect(!result.success, "empty path must fail");
    expect(result.win32_error == ERROR_INVALID_PARAMETER,
           "empty path must report ERROR_INVALID_PARAMETER");
}

void test_existing_file() {
    wchar_t tempPath[MAX_PATH]{};
    wchar_t tempFile[MAX_PATH]{};

    const DWORD pathLength = ::GetTempPathW(MAX_PATH, tempPath);
    expect(pathLength > 0 && pathLength < MAX_PATH, "GetTempPathW must succeed");
    if (pathLength == 0 || pathLength >= MAX_PATH) return;

    expect(::GetTempFileNameW(tempPath, L"csb", 0, tempFile) != 0,
           "GetTempFileNameW must create a temporary file");
    if (tempFile[0] == L'\0') return;

    const auto result =
        craybridge::file_time::query_creation_time_unix_milliseconds(tempFile);
    expect(result.success, "existing file creation time must be readable");
    expect(result.win32_error == ERROR_SUCCESS,
           "successful file query must report ERROR_SUCCESS");
    expect(result.unix_milliseconds > 0,
           "temporary file creation timestamp must be positive");

    ::DeleteFileW(tempFile);
}

void test_missing_file() {
    wchar_t tempPath[MAX_PATH]{};
    wchar_t tempFile[MAX_PATH]{};

    const DWORD pathLength = ::GetTempPathW(MAX_PATH, tempPath);
    if (pathLength == 0 || pathLength >= MAX_PATH) {
        expect(false, "GetTempPathW must succeed for missing-file test");
        return;
    }

    if (::GetTempFileNameW(tempPath, L"csb", 0, tempFile) == 0) {
        expect(false, "GetTempFileNameW must create path for missing-file test");
        return;
    }

    ::DeleteFileW(tempFile);
    const auto result =
        craybridge::file_time::query_creation_time_unix_milliseconds(tempFile);
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

    const auto result =
        craybridge::file_time::query_creation_time_unix_milliseconds(tempPath);
    expect(!result.success, "directory must be rejected");
    expect(result.win32_error == ERROR_DIRECTORY,
           "directory must report ERROR_DIRECTORY");
}

} // namespace

int wmain() {
    test_epoch_conversion();
    test_empty_path();
    test_existing_file();
    test_missing_file();
    test_directory_is_rejected();

    if (g_failures != 0) {
        std::cerr << g_failures << " test(s) failed.\n";
        return 1;
    }

    std::cout << "All file creation time tests passed.\n";
    return 0;
}
