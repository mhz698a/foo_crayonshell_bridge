#include "../src/timestamp_job_manager.h"

#include <Windows.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

bool create_temp_file(wchar_t (&tempFile)[MAX_PATH]) {
    wchar_t tempPath[MAX_PATH]{};
    const DWORD length = ::GetTempPathW(MAX_PATH, tempPath);
    if (length == 0 || length >= MAX_PATH) return false;
    return ::GetTempFileNameW(tempPath, L"csj", 0, tempFile) != 0;
}

void test_async_timestamp_write_and_query() {
    wchar_t tempFile[MAX_PATH]{};
    expect(create_temp_file(tempFile),
           "job test must create a temporary media file");
    if (tempFile[0] == L'\0') return;

    craybridge::api::TimestampJobManager manager;
    expect(manager.start(), "timestamp job manager must start");

    craybridge::file_time::TimestampUpdate update{};
    update.creation_time = 1700000000000ULL;
    update.last_write_time = 1700000001000ULL;

    std::string jobId;
    const auto submitted = manager.submit(
        tempFile, 3, 7, update, jobId);
    expect(submitted == craybridge::api::TimestampJobSubmitStatus::Accepted,
           "valid timestamp update must be accepted as a job");
    expect(jobId.size() == 32,
           "accepted job must return a 128-bit opaque identifier");

    if (submitted == craybridge::api::TimestampJobSubmitStatus::Accepted) {
        craybridge::api::TimestampJobSnapshot snapshot{};
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);

        bool found = false;
        while (std::chrono::steady_clock::now() < deadline) {
            found = manager.query(jobId, snapshot);
            if (found && snapshot.status != "queued" &&
                snapshot.status != "running") {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        expect(found, "submitted job must be queryable");
        expect(snapshot.status == "succeeded",
               "timestamp write job must complete with verified readback");
        expect(snapshot.has_result && snapshot.result.success,
               "job result must report SetFileTime success");
        expect(snapshot.result.readback_success,
               "job result must report successful timestamp readback");
        expect(snapshot.playlist_index == 3 && snapshot.item_index == 7,
               "job must preserve the original playlist coordinates");

        expect(manager.cancel(jobId) ==
                   craybridge::api::TimestampJobCancelStatus::AlreadyCompleted,
               "completed jobs must not be cancellable");
    }

    manager.stop();
    ::DeleteFileW(tempFile);
}

void test_unknown_job_cannot_be_cancelled() {
    craybridge::api::TimestampJobManager manager;
    expect(manager.start(), "timestamp job manager must start for cancel test");
    expect(manager.cancel("00000000000000000000000000000000") ==
               craybridge::api::TimestampJobCancelStatus::NotFound,
           "unknown job identifier must not be cancellable");
    manager.stop();
}

} // namespace

int run_timestamp_job_manager_tests() {
    test_async_timestamp_write_and_query();
    test_unknown_job_cannot_be_cancelled();

    if (g_failures != 0) {
        std::cerr << g_failures << " timestamp job test(s) failed.\n";
        return 1;
    }

    std::cout << "All timestamp job manager tests passed.\n";
    return 0;
}
