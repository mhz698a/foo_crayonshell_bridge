#pragma once

#include "http_request_parser.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace craybridge::http {

struct HttpResponse {
    unsigned int status_code = 200;
    std::string body;
};

struct HttpServerOptions {
    std::size_t worker_count = 4;
    std::size_t queue_capacity = 16;
    std::uint32_t socket_timeout_ms = 3000;
    bool allow_vite_dev_origin = false;
};

struct HttpServerStartResult {
    bool success = false;
    std::uint16_t port = 0;
    std::string bearer_token;
    unsigned long error_code = 0;
    std::string error_message;
};

using RequestHandler = std::function<HttpResponse(const Request&)>;

// Small, loopback-only HTTP/1.1 server. The handler is called on one of the
// bounded worker threads and must remain short/non-blocking. Long-running SDK
// or filesystem work must be submitted to dedicated bounded queues/jobs.
class HttpServer final {
public:
    explicit HttpServer(RequestHandler handler,
                        HttpServerOptions options = {});
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;
    HttpServer(HttpServer&&) = delete;
    HttpServer& operator=(HttpServer&&) = delete;

    [[nodiscard]] HttpServerStartResult start();
    void stop() noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] std::size_t queued_connections() const noexcept;
    [[nodiscard]] std::size_t active_connections() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace craybridge::http
