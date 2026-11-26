#pragma once

#include "file_creation_time.h"

#include <string_view>

namespace craybridge::api {

enum class TimestampUpdateJsonError {
    None,
    InvalidJson,
    UnknownField,
    DuplicateField,
    UnsupportedField,
    InvalidTimestamp
};

[[nodiscard]] bool parse_timestamp_update_json(
    std::string_view json,
    file_time::TimestampUpdate& output,
    TimestampUpdateJsonError& error) noexcept;

} // namespace craybridge::api
