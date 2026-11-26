#include "timestamp_update_json.h"

#include <charconv>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace craybridge::api {
namespace {

class TimestampJsonReader final {
public:
    explicit TimestampJsonReader(std::string_view input) noexcept
        : input_(input) {}

    bool parse(file_time::TimestampUpdate& output,
               TimestampUpdateJsonError& error) noexcept {
        output = {};
        error = TimestampUpdateJsonError::None;

        skip_whitespace();
        if (!consume('{')) return fail(error, TimestampUpdateJsonError::InvalidJson);
        skip_whitespace();

        if (consume('}')) {
            return fail(error, TimestampUpdateJsonError::InvalidJson);
        }

        bool saw_field = false;
        bool saw_creation = false;
        bool saw_access = false;
        bool saw_write = false;

        for (;;) {
            std::string_view key;
            if (!parse_key(key)) {
                return fail(error, TimestampUpdateJsonError::InvalidJson);
            }

            skip_whitespace();
            if (!consume(':')) {
                return fail(error, TimestampUpdateJsonError::InvalidJson);
            }
            skip_whitespace();

            if (key == "changeTime") {
                return fail(error, TimestampUpdateJsonError::UnsupportedField);
            }

            bool* seen = nullptr;
            std::optional<std::uint64_t>* destination = nullptr;

            if (key == "creationTime") {
                seen = &saw_creation;
                destination = &output.creation_time;
            } else if (key == "lastAccessTime") {
                seen = &saw_access;
                destination = &output.last_access_time;
            } else if (key == "lastWriteTime") {
                seen = &saw_write;
                destination = &output.last_write_time;
            } else {
                return fail(error, TimestampUpdateJsonError::UnknownField);
            }

            if (*seen) {
                return fail(error, TimestampUpdateJsonError::DuplicateField);
            }

            std::uint64_t milliseconds = 0;
            if (!parse_unsigned_integer(milliseconds) ||
                !file_time::unix_milliseconds_to_filetime_ticks(milliseconds)
                     .has_value()) {
                return fail(error, TimestampUpdateJsonError::InvalidTimestamp);
            }

            *destination = milliseconds;
            *seen = true;
            saw_field = true;

            skip_whitespace();
            if (consume('}')) break;
            if (!consume(',')) {
                return fail(error, TimestampUpdateJsonError::InvalidJson);
            }
            skip_whitespace();
        }

        skip_whitespace();
        if (position_ != input_.size() || !saw_field) {
            return fail(error, TimestampUpdateJsonError::InvalidJson);
        }
        return true;
    }

private:
    bool fail(TimestampUpdateJsonError& error,
              TimestampUpdateJsonError value) noexcept {
        error = value;
        return false;
    }

    void skip_whitespace() noexcept {
        while (position_ < input_.size()) {
            const char ch = input_[position_];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
            ++position_;
        }
    }

    bool consume(char expected) noexcept {
        if (position_ >= input_.size() || input_[position_] != expected) {
            return false;
        }
        ++position_;
        return true;
    }

    bool parse_key(std::string_view& output) noexcept {
        if (!consume('"')) return false;
        const auto start = position_;

        while (position_ < input_.size()) {
            const unsigned char ch =
                static_cast<unsigned char>(input_[position_]);
            if (ch == '"') {
                output = input_.substr(start, position_ - start);
                ++position_;
                return !output.empty();
            }
            if (ch < 0x20 || ch == '\\') return false;
            ++position_;
        }
        return false;
    }

    bool parse_unsigned_integer(std::uint64_t& output) noexcept {
        const auto start = position_;
        while (position_ < input_.size()) {
            const char ch = input_[position_];
            if (ch < '0' || ch > '9') break;
            ++position_;
        }

        if (position_ == start) return false;
        if (position_ - start > 1 && input_[start] == '0') return false;

        const auto parsed = std::from_chars(
            input_.data() + start,
            input_.data() + position_,
            output,
            10);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != input_.data() + position_) {
            return false;
        }

        if (position_ < input_.size()) {
            const char next = input_[position_];
            if (next != ' ' && next != '\t' && next != '\r' &&
                next != '\n' && next != ',' && next != '}') {
                return false;
            }
        }
        return true;
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

} // namespace

bool parse_timestamp_update_json(
    std::string_view json,
    file_time::TimestampUpdate& output,
    TimestampUpdateJsonError& error) noexcept {
    try {
        TimestampJsonReader reader(json);
        return reader.parse(output, error);
    } catch (...) {
        output = {};
        error = TimestampUpdateJsonError::InvalidJson;
        return false;
    }
}

} // namespace craybridge::api
