#pragma once

#include "file_creation_time.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace craybridge::api {

enum class TimestampJobSubmitStatus {
    Accepted,
    QueueFull,
    ServiceStopping,
    IdentifierFailure,
    InternalError
};

enum class TimestampJobCancelStatus {
    Cancelled,
    Running,
    AlreadyCompleted,
    NotFound
};

struct TimestampJobSnapshot {
    std::string id;
    std::string status;
    std::uint64_t playlist_index = 0;
    std::uint64_t item_index = 0;
    file_time::TimestampUpdate requested;
    bool has_result = false;
    file_time::TimestampWriteResult result;
};

class TimestampJobManager final {
public:
    TimestampJobManager();
    ~TimestampJobManager();

    TimestampJobManager(const TimestampJobManager&) = delete;
    TimestampJobManager& operator=(const TimestampJobManager&) = delete;

    [[nodiscard]] bool start() noexcept;
    void stop() noexcept;

    [[nodiscard]] TimestampJobSubmitStatus submit(
        std::wstring physical_path,
        std::uint64_t playlist_index,
        std::uint64_t item_index,
        const file_time::TimestampUpdate& update,
        std::string& job_id) noexcept;

    [[nodiscard]] bool query(
        std::string_view job_id,
        TimestampJobSnapshot& output) const noexcept;

    [[nodiscard]] TimestampJobCancelStatus cancel(
        std::string_view job_id) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace craybridge::api
