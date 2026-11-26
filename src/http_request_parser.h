#pragma once

#include <llhttp.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace craybridge::http {

inline constexpr std::size_t kMaxRequestHeaderBytes = 16 * 1024;
inline constexpr std::size_t kMaxRequestBodyBytes = 16 * 1024;
inline constexpr std::size_t kMaxRequestBytes =
    kMaxRequestHeaderBytes + kMaxRequestBodyBytes;
inline constexpr std::size_t kMaxRequestTargetBytes = 4096;
inline constexpr std::size_t kMaxRequestMethodBytes = 32;
inline constexpr std::size_t kMaxRequestHeaderCount = 64;

struct Header {
    std::string name;
    std::string value;
};

struct Request {
    std::string method;
    std::string target;
    std::uint8_t http_major = 0;
    std::uint8_t http_minor = 0;
    std::vector<Header> headers;
    std::string body;
};

enum class ParseState {
    NeedMoreData,
    Complete,
    Error
};

struct ParseResult {
    ParseState state = ParseState::NeedMoreData;
    const char* error_code = nullptr;
};

class RequestParser final {
public:
    RequestParser();
    ~RequestParser() = default;

    RequestParser(const RequestParser&) = delete;
    RequestParser& operator=(const RequestParser&) = delete;
    RequestParser(RequestParser&&) = delete;
    RequestParser& operator=(RequestParser&&) = delete;

    [[nodiscard]] ParseResult execute(const char* data,
                                      std::size_t size) noexcept;
    [[nodiscard]] ParseResult finish() noexcept;

    [[nodiscard]] const Request& request() const noexcept {
        return request_;
    }

    [[nodiscard]] bool complete() const noexcept {
        return complete_;
    }

    [[nodiscard]] const char* error_message() const noexcept {
        return error_message_;
    }

private:
    static RequestParser* from(llhttp_t* parser) noexcept;
    static int on_method(llhttp_t* parser,
                         const char* data,
                         std::size_t size) noexcept;
    static int on_url(llhttp_t* parser,
                      const char* data,
                      std::size_t size) noexcept;
    static int on_header_field(llhttp_t* parser,
                               const char* data,
                               std::size_t size) noexcept;
    static int on_header_value(llhttp_t* parser,
                               const char* data,
                               std::size_t size) noexcept;
    static int on_header_value_complete(llhttp_t* parser) noexcept;
    static int on_headers_complete(llhttp_t* parser) noexcept;
    static int on_body(llhttp_t* parser,
                       const char* data,
                       std::size_t size) noexcept;
    static int on_message_complete(llhttp_t* parser) noexcept;

    [[nodiscard]] int append_method(const char* data,
                                    std::size_t size) noexcept;
    [[nodiscard]] int append_target(const char* data,
                                    std::size_t size) noexcept;
    [[nodiscard]] int append_header_field(const char* data,
                                          std::size_t size) noexcept;
    [[nodiscard]] int append_header_value(const char* data,
                                          std::size_t size) noexcept;
    [[nodiscard]] int append_body(const char* data,
                                  std::size_t size) noexcept;
    [[nodiscard]] int fail(llhttp_t* parser,
                           const char* reason) noexcept;

    llhttp_t parser_{};
    llhttp_settings_t settings_{};
    Request request_;
    std::string current_header_field_;
    std::string current_header_value_;
    std::size_t header_bytes_ = 0;
    std::size_t received_bytes_ = 0;
    const char* error_message_ = nullptr;
    bool complete_ = false;
    bool failed_ = false;
};

} // namespace craybridge::http
