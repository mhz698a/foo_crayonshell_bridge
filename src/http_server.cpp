#include "http_server.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace craybridge::http {
namespace {

constexpr std::size_t kReceiveBufferBytes = 4096;
constexpr std::size_t kMaxResponseBodyBytes = 64 * 1024;
constexpr DWORD kListenerPollMilliseconds = 100;
constexpr char kProductionOrigin[] = "https://foo-ui-webview2.local";
constexpr char kDevelopmentOrigin[] = "http://localhost:5173";

struct HeaderValue {
    const std::string* value = nullptr;
    bool duplicate = false;
};

bool ascii_equal_ci(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;

    for (std::size_t i = 0; i < left.size(); ++i) {
        char a = left[i];
        char b = right[i];

        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');

        if (a != b) return false;
    }

    return true;
}

HeaderValue find_header(const Request& request,
                        std::string_view name) noexcept {
    HeaderValue result{};

    for (const auto& header : request.headers) {
        if (!ascii_equal_ci(header.name, name)) continue;

        if (result.value != nullptr) {
            result.duplicate = true;
            return result;
        }

        result.value = &header.value;
    }

    return result;
}

bool constant_time_equal(std::string_view left,
                         std::string_view right) noexcept {
    const std::size_t maximum = (std::max)(left.size(), right.size());
    unsigned int difference =
        static_cast<unsigned int>(left.size() ^ right.size());

    for (std::size_t i = 0; i < maximum; ++i) {
        const unsigned char a = i < left.size()
            ? static_cast<unsigned char>(left[i]) : 0;
        const unsigned char b = i < right.size()
            ? static_cast<unsigned char>(right[i]) : 0;
        difference |= static_cast<unsigned int>(a ^ b);
    }

    return difference == 0;
}

std::string base64url_encode(const unsigned char* bytes,
                             std::size_t size) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    std::string encoded;
    encoded.reserve((size * 4 + 2) / 3);

    for (std::size_t offset = 0; offset < size; offset += 3) {
        const std::size_t remaining = size - offset;
        const std::uint32_t a = bytes[offset];
        const std::uint32_t b = remaining > 1 ? bytes[offset + 1] : 0;
        const std::uint32_t c = remaining > 2 ? bytes[offset + 2] : 0;
        const std::uint32_t value = (a << 16) | (b << 8) | c;

        encoded.push_back(alphabet[(value >> 18) & 0x3f]);
        encoded.push_back(alphabet[(value >> 12) & 0x3f]);

        if (remaining > 1) {
            encoded.push_back(alphabet[(value >> 6) & 0x3f]);
        }
        if (remaining > 2) {
            encoded.push_back(alphabet[value & 0x3f]);
        }
    }

    return encoded;
}

bool generate_bearer_token(std::string& token,
                           unsigned long& error_code) {
    std::array<unsigned char, 32> random_bytes{};
    const NTSTATUS status = ::BCryptGenRandom(
        nullptr,
        random_bytes.data(),
        static_cast<ULONG>(random_bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    if (status != 0) {
        error_code = static_cast<unsigned long>(status);
        return false;
    }

    try {
        token = base64url_encode(random_bytes.data(), random_bytes.size());
    } catch (...) {
        error_code = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }

    error_code = ERROR_SUCCESS;
    return true;
}

bool send_all(SOCKET socket,
              std::string_view bytes,
              DWORD timeout_ms = 3000) noexcept {
    const ULONGLONG deadline = ::GetTickCount64() + timeout_ms;
    std::size_t sent = 0;

    while (sent < bytes.size()) {
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline) return false;

        const DWORD remaining_timeout =
            static_cast<DWORD>(deadline - now);
        ::setsockopt(
            socket, SOL_SOCKET, SO_SNDTIMEO,
            reinterpret_cast<const char*>(&remaining_timeout),
            sizeof(remaining_timeout));

        const auto remaining = bytes.size() - sent;
        const int chunk = static_cast<int>(
            (std::min)(remaining,
                       static_cast<std::size_t>((std::numeric_limits<int>::max)())));

        const int result = ::send(
            socket,
            bytes.data() + sent,
            chunk,
            0);

        if (result == SOCKET_ERROR || result == 0) {
            return false;
        }

        sent += static_cast<std::size_t>(result);
    }

    return true;
}

const char* reason_phrase(unsigned int status) noexcept {
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Content";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return "Internal Server Error";
    }
}

std::string error_json(std::string_view code,
                       std::string_view message) {
    // All current call sites pass fixed, source-controlled strings.
    return std::string("{\"success\":false,\"error\":{\"code\":\"") +
        std::string(code) + "\",\"message\":\"" +
        std::string(message) + "\"}}";
}

bool is_allowed_preflight_method(std::string_view method) noexcept {
    return method == "GET" || method == "PATCH" || method == "DELETE";
}

bool validate_preflight_headers(std::string_view value) {
    std::size_t offset = 0;

    while (offset < value.size()) {
        const auto comma = value.find(',', offset);
        const auto end = comma == std::string_view::npos
            ? value.size() : comma;
        auto token = value.substr(offset, end - offset);

        while (!token.empty() &&
               (token.front() == ' ' || token.front() == '\t')) {
            token.remove_prefix(1);
        }
        while (!token.empty() &&
               (token.back() == ' ' || token.back() == '\t')) {
            token.remove_suffix(1);
        }

        if (!token.empty() &&
            !ascii_equal_ci(token, "authorization") &&
            !ascii_equal_ci(token, "content-type")) {
            return false;
        }

        if (comma == std::string_view::npos) break;
        offset = comma + 1;
    }

    return true;
}

} // namespace

struct HttpServer::Impl {
    explicit Impl(RequestHandler requestHandler, HttpServerOptions serverOptions)
        : handler(std::move(requestHandler)),
          options(serverOptions) {}

    ~Impl() {
        stop();
    }

    RequestHandler handler;
    HttpServerOptions options;

    mutable std::mutex lifecycle_mutex;
    std::atomic<bool> is_running{false};
    std::atomic<bool> stopping{false};
    std::atomic<std::uint16_t> bound_port{0};

    WSADATA winsock_data{};
    bool winsock_started = false;
    SOCKET listen_socket = INVALID_SOCKET;
    std::string bearer_token;
    std::string expected_host;
    std::vector<std::string> allowed_origins;

    std::thread listener_thread;
    std::vector<std::thread> workers;

    mutable std::mutex queue_mutex;
    std::condition_variable queue_condition;
    std::deque<SOCKET> pending_sockets;
    std::vector<SOCKET> active_sockets;

    HttpServerStartResult start() {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);

        HttpServerStartResult result{};
        if (is_running.load() || winsock_started ||
            listen_socket != INVALID_SOCKET ||
            listener_thread.joinable() || !workers.empty()) {
            result.error_code = ERROR_ALREADY_INITIALIZED;
            result.error_message = "The local HTTP service is already running or awaiting shutdown.";
            return result;
        }

        if (options.worker_count == 0 || options.worker_count > 8 ||
            options.queue_capacity == 0 || options.queue_capacity > 64 ||
            options.socket_timeout_ms < 250 ||
            options.socket_timeout_ms > 10000) {
            result.error_code = ERROR_INVALID_PARAMETER;
            result.error_message = "The local HTTP service configuration is invalid.";
            return result;
        }

        int wsa_result = ::WSAStartup(MAKEWORD(2, 2), &winsock_data);
        if (wsa_result != 0) {
            result.error_code = static_cast<unsigned long>(wsa_result);
            result.error_message = "Winsock initialization failed.";
            return result;
        }
        winsock_started = true;

        listen_socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_socket == INVALID_SOCKET) {
            result.error_code = static_cast<unsigned long>(::WSAGetLastError());
            result.error_message = "Unable to create the loopback socket.";
            cleanup_failed_start();
            return result;
        }

        const BOOL exclusive_address_use = TRUE;
        if (::setsockopt(
                listen_socket,
                SOL_SOCKET,
                SO_EXCLUSIVEADDRUSE,
                reinterpret_cast<const char*>(&exclusive_address_use),
                sizeof(exclusive_address_use)) == SOCKET_ERROR) {
            result.error_code = static_cast<unsigned long>(::WSAGetLastError());
            result.error_message = "Unable to reserve the loopback socket exclusively.";
            cleanup_failed_start();
            return result;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;

        if (::bind(
                listen_socket,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == SOCKET_ERROR) {
            result.error_code = static_cast<unsigned long>(::WSAGetLastError());
            result.error_message = "Unable to bind the IPv4 loopback address.";
            cleanup_failed_start();
            return result;
        }

        if (::listen(
                listen_socket,
                static_cast<int>(options.worker_count + options.queue_capacity))
                == SOCKET_ERROR) {
            result.error_code = static_cast<unsigned long>(::WSAGetLastError());
            result.error_message = "Unable to listen on the loopback socket.";
            cleanup_failed_start();
            return result;
        }

        u_long non_blocking = 1;
        if (::ioctlsocket(listen_socket, FIONBIO, &non_blocking) == SOCKET_ERROR) {
            result.error_code = static_cast<unsigned long>(::WSAGetLastError());
            result.error_message = "Unable to configure the listener socket.";
            cleanup_failed_start();
            return result;
        }

        sockaddr_in bound_address{};
        int bound_length = sizeof(bound_address);
        if (::getsockname(
                listen_socket,
                reinterpret_cast<sockaddr*>(&bound_address),
                &bound_length) == SOCKET_ERROR) {
            result.error_code = static_cast<unsigned long>(::WSAGetLastError());
            result.error_message = "Unable to determine the ephemeral service port.";
            cleanup_failed_start();
            return result;
        }

        unsigned long random_error = ERROR_SUCCESS;
        if (!generate_bearer_token(bearer_token, random_error)) {
            result.error_code = random_error;
            result.error_message = "Unable to generate the service authentication token.";
            cleanup_failed_start();
            return result;
        }

        const auto selected_port = ntohs(bound_address.sin_port);
        bound_port.store(selected_port);

        try {
            expected_host = "127.0.0.1:" + std::to_string(selected_port);
            allowed_origins.clear();
            allowed_origins.emplace_back(kProductionOrigin);
            if (options.allow_vite_dev_origin) {
                allowed_origins.emplace_back(kDevelopmentOrigin);
            }
        } catch (...) {
            result.error_code = ERROR_NOT_ENOUGH_MEMORY;
            result.error_message = "Unable to initialize the local service security policy.";
            cleanup_failed_start();
            return result;
        }

        stopping.store(false);
        is_running.store(true);

        try {
            workers.reserve(options.worker_count);
            active_sockets.reserve(options.worker_count);
            for (std::size_t i = 0; i < options.worker_count; ++i) {
                workers.emplace_back([this] { worker_loop(); });
            }

            listener_thread = std::thread([this] { listener_loop(); });
        } catch (...) {
            result.error_code = ERROR_NOT_ENOUGH_MEMORY;
            result.error_message = "Unable to create the bounded HTTP worker threads.";
            is_running.store(false);
            stopping.store(true);
            queue_condition.notify_all();

            for (auto& worker : workers) {
                if (worker.joinable()) worker.join();
            }
            workers.clear();
            cleanup_failed_start();
            return result;
        }

        result.success = true;
        result.port = selected_port;
        result.bearer_token = bearer_token;
        result.error_code = ERROR_SUCCESS;
        return result;
    }

    void cleanup_failed_start() noexcept {
        if (listen_socket != INVALID_SOCKET) {
            ::shutdown(listen_socket, SD_BOTH);
            ::closesocket(listen_socket);
            listen_socket = INVALID_SOCKET;
        }

        if (winsock_started) {
            ::WSACleanup();
            winsock_started = false;
        }

        bearer_token.clear();
        expected_host.clear();
        allowed_origins.clear();
        bound_port.store(0);
        is_running.store(false);
        stopping.store(false);
    }

    void stop() noexcept {
        std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex);

        if (!winsock_started &&
            listen_socket == INVALID_SOCKET &&
            !listener_thread.joinable() &&
            workers.empty()) {
            return;
        }

        stopping.store(true);

        {
            std::lock_guard<std::mutex> queue_lock(queue_mutex);

            for (const SOCKET pending : pending_sockets) {
                ::shutdown(pending, SD_BOTH);
                ::closesocket(pending);
            }
            pending_sockets.clear();

            for (const SOCKET active : active_sockets) {
                ::shutdown(active, SD_BOTH);
            }
        }

        queue_condition.notify_all();

        // The listener polls select() with a bounded timeout. It owns no
        // accepted sockets while blocked, so stop never closes its socket
        // from a different thread.
        if (listener_thread.joinable()) {
            listener_thread.join();
        }

        if (listen_socket != INVALID_SOCKET) {
            ::shutdown(listen_socket, SD_BOTH);
            ::closesocket(listen_socket);
            listen_socket = INVALID_SOCKET;
        }

        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }
        workers.clear();

        if (winsock_started) {
            ::WSACleanup();
            winsock_started = false;
        }

        {
            std::lock_guard<std::mutex> queue_lock(queue_mutex);
            pending_sockets.clear();
            active_sockets.clear();
        }

        bearer_token.clear();
        expected_host.clear();
        allowed_origins.clear();
        bound_port.store(0);
        is_running.store(false);
        stopping.store(false);
    }

    void request_shutdown_from_listener() noexcept {
        stopping.store(true);

        {
            std::lock_guard<std::mutex> queue_lock(queue_mutex);

            for (const SOCKET pending : pending_sockets) {
                ::shutdown(pending, SD_BOTH);
                ::closesocket(pending);
            }
            pending_sockets.clear();

            for (const SOCKET active : active_sockets) {
                ::shutdown(active, SD_BOTH);
            }
        }

        is_running.store(false);
        queue_condition.notify_all();
    }

    void listener_loop() noexcept {
        while (!stopping.load()) {
            fd_set read_set;
            FD_ZERO(&read_set);
            FD_SET(listen_socket, &read_set);

            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = static_cast<long>(kListenerPollMilliseconds) * 1000L;

            const int ready = ::select(0, &read_set, nullptr, nullptr, &timeout);
            if (ready == 0) {
                continue;
            }

            if (ready == SOCKET_ERROR) {
                if (!stopping.load()) {
                    request_shutdown_from_listener();
                }
                return;
            }

            sockaddr_in peer{};
            int peer_length = sizeof(peer);
            const SOCKET accepted = ::accept(
                listen_socket,
                reinterpret_cast<sockaddr*>(&peer),
                &peer_length);

            if (accepted == INVALID_SOCKET) {
                const int error = ::WSAGetLastError();
                if (error == WSAEWOULDBLOCK ||
                    error == WSAEINTR ||
                    error == WSAECONNRESET) {
                    continue;
                }

                if (!stopping.load()) {
                    request_shutdown_from_listener();
                }
                return;
            }

            if (peer.sin_family != AF_INET ||
                peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
                ::shutdown(accepted, SD_BOTH);
                ::closesocket(accepted);
                continue;
            }

            // accept() returns a socket with the listener's operating
            // properties. The listener is non-blocking for select/accept;
            // explicitly restore blocking mode for worker recv/send so the
            // configured SO_RCVTIMEO/SO_SNDTIMEO deadlines are effective.
            u_long blocking_mode = 0;
            if (::ioctlsocket(accepted, FIONBIO, &blocking_mode) == SOCKET_ERROR) {
                ::shutdown(accepted, SD_BOTH);
                ::closesocket(accepted);
                continue;
            }

            const DWORD timeout_ms = options.socket_timeout_ms;
            if (::setsockopt(
                    accepted, SOL_SOCKET, SO_RCVTIMEO,
                    reinterpret_cast<const char*>(&timeout_ms),
                    sizeof(timeout_ms)) == SOCKET_ERROR ||
                ::setsockopt(
                    accepted, SOL_SOCKET, SO_SNDTIMEO,
                    reinterpret_cast<const char*>(&timeout_ms),
                    sizeof(timeout_ms)) == SOCKET_ERROR) {
                ::shutdown(accepted, SD_BOTH);
                ::closesocket(accepted);
                continue;
            }

            bool queued = false;
            bool saturated = false;
            {
                std::lock_guard<std::mutex> queue_lock(queue_mutex);

                if (stopping.load()) {
                    // The socket is closed after leaving the queue lock.
                } else if (pending_sockets.size() >= options.queue_capacity) {
                    saturated = true;
                } else {
                    try {
                        pending_sockets.push_back(accepted);
                        queued = true;
                    } catch (...) {
                        saturated = true;
                    }
                }
            }

            if (queued) {
                queue_condition.notify_one();
            } else if (saturated) {
                send_overload_response(accepted);
                ::shutdown(accepted, SD_BOTH);
                ::closesocket(accepted);
            } else {
                ::shutdown(accepted, SD_BOTH);
                ::closesocket(accepted);
            }
        }
    }

    static void send_overload_response(SOCKET socket) noexcept {
        static constexpr std::string_view response =
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Content-Type: application/json; charset=utf-8\r\n"
            "Content-Length: 88\r\n"
            "Connection: close\r\n"
            "Cache-Control: no-store\r\n"
            "\r\n"
            "{\"success\":false,\"error\":{\"code\":\"SERVICE_BUSY\","
            "\"message\":\"The local service is busy.\"}}";
        send_all(socket, response, 100);
    }

    void worker_loop() noexcept {
        for (;;) {
            SOCKET accepted = INVALID_SOCKET;

            {
                std::unique_lock<std::mutex> queue_lock(queue_mutex);
                queue_condition.wait(queue_lock, [this] {
                    return stopping.load() || !pending_sockets.empty();
                });

                if (stopping.load() && pending_sockets.empty()) {
                    return;
                }

                accepted = pending_sockets.front();
                pending_sockets.pop_front();
                active_sockets.push_back(accepted);
            }

            process_socket(accepted);

            // Remove the descriptor from the stop-visible set before closing
            // it. Otherwise stop() could shutdown a recycled SOCKET value.
            {
                std::lock_guard<std::mutex> queue_lock(queue_mutex);
                const auto active = std::find(
                    active_sockets.begin(), active_sockets.end(), accepted);
                if (active != active_sockets.end()) {
                    active_sockets.erase(active);
                }
            }

            ::shutdown(accepted, SD_BOTH);
            ::closesocket(accepted);
        }
    }

    void process_socket(SOCKET socket) noexcept {
        RequestParser parser;
        std::array<char, kReceiveBufferBytes> buffer{};
        const ULONGLONG deadline =
            ::GetTickCount64() + options.socket_timeout_ms;

        for (;;) {
            const ULONGLONG now = ::GetTickCount64();
            if (now >= deadline) {
                send_error(socket, 408, "REQUEST_TIMEOUT",
                           "The HTTP request timed out.");
                return;
            }

            const DWORD remaining_timeout =
                static_cast<DWORD>(deadline - now);
            ::setsockopt(
                socket, SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char*>(&remaining_timeout),
                sizeof(remaining_timeout));

            const int received = ::recv(
                socket,
                buffer.data(),
                static_cast<int>(buffer.size()),
                0);

            if (received > 0) {
                const auto result = parser.execute(
                    buffer.data(),
                    static_cast<std::size_t>(received));

                if (result.state == ParseState::Complete) {
                    dispatch_request(socket, parser.request());
                    return;
                }

                if (result.state == ParseState::Error) {
                    send_error(socket, 400, "INVALID_REQUEST",
                               "The HTTP request is malformed.");
                    return;
                }

                continue;
            }

            if (received == 0) {
                const auto result = parser.finish();
                if (result.state == ParseState::Complete) {
                    dispatch_request(socket, parser.request());
                } else {
                    send_error(socket, 400, "INVALID_REQUEST",
                               "The HTTP request ended before it was complete.");
                }
                return;
            }

            const int socket_error = ::WSAGetLastError();
            if (socket_error == WSAETIMEDOUT) {
                send_error(socket, 408, "REQUEST_TIMEOUT",
                           "The HTTP request timed out.");
            } else if (!stopping.load()) {
                send_error(socket, 400, "INVALID_REQUEST",
                           "The HTTP request could not be received.");
            }
            return;
        }
    }

    bool is_allowed_origin(std::string_view origin) const noexcept {
        for (const auto& allowed : allowed_origins) {
            if (origin == allowed) return true;
        }
        return false;
    }

    void dispatch_request(SOCKET socket, const Request& request) noexcept {
        const auto host = find_header(request, "Host");
        if (host.duplicate || host.value == nullptr ||
            !ascii_equal_ci(*host.value, expected_host)) {
            send_error(socket, 421, "INVALID_HOST",
                       "The request Host header is not allowed.");
            return;
        }

        const auto origin = find_header(request, "Origin");
        if (origin.duplicate || origin.value == nullptr ||
            !is_allowed_origin(*origin.value)) {
            send_error(socket, 403, "ORIGIN_NOT_ALLOWED",
                       "The request origin is not allowed.");
            return;
        }

        if (request.method == "OPTIONS") {
            handle_preflight(socket, request, *origin.value);
            return;
        }

        const auto authorization = find_header(request, "Authorization");
        if (authorization.duplicate ||
            authorization.value == nullptr ||
            !valid_bearer(*authorization.value)) {
            send_response(
                socket,
                401,
                error_json("UNAUTHORIZED",
                           "A valid service bearer token is required."),
                *origin.value,
                false,
                true);
            return;
        }

        HttpResponse response{};
        try {
            if (handler) {
                response = handler(request);
            } else {
                response.status_code = 501;
                response.body = error_json(
                    "NOT_IMPLEMENTED",
                    "No HTTP API handler is registered.");
            }
        } catch (...) {
            response.status_code = 500;
            response.body = error_json(
                "INTERNAL_ERROR",
                "The local service could not process the request.");
        }

        if (response.status_code < 200 || response.status_code > 599 ||
            response.body.size() > kMaxResponseBodyBytes) {
            response.status_code = 500;
            response.body = error_json(
                "INTERNAL_ERROR",
                "The local service produced an invalid response.");
        }

        send_response(socket, response.status_code, response.body, *origin.value);
    }

    bool valid_bearer(std::string_view value) const noexcept {
        constexpr std::string_view kPrefix = "Bearer ";
        if (value.size() <= kPrefix.size() ||
            !ascii_equal_ci(value.substr(0, kPrefix.size()), kPrefix)) {
            return false;
        }

        const auto supplied = value.substr(kPrefix.size());
        if (supplied.find_first_of(" \t\r\n") != std::string_view::npos) {
            return false;
        }

        return constant_time_equal(supplied, bearer_token);
    }

    void handle_preflight(SOCKET socket,
                          const Request& request,
                          const std::string& origin) noexcept {
        const auto requested_method =
            find_header(request, "Access-Control-Request-Method");
        const auto requested_headers =
            find_header(request, "Access-Control-Request-Headers");

        if (requested_method.duplicate ||
            requested_method.value == nullptr ||
            !is_allowed_preflight_method(*requested_method.value) ||
            requested_headers.duplicate ||
            (requested_headers.value != nullptr &&
             !validate_preflight_headers(*requested_headers.value))) {
            send_response(
                socket,
                403,
                error_json("PREFLIGHT_NOT_ALLOWED",
                           "The requested cross-origin operation is not allowed."),
                origin);
            return;
        }

        send_response(socket, 204, {}, origin, true);
    }

    void send_error(SOCKET socket,
                    unsigned int status,
                    std::string_view code,
                    std::string_view message) noexcept {
        try {
            send_response(socket, status, error_json(code, message));
        } catch (...) {
            // A failed error response must not terminate a worker thread.
        }
    }

    void send_response(SOCKET socket,
                       unsigned int status,
                       const std::string& body,
                       std::string_view origin = {},
                       bool preflight = false,
                       bool unauthorized = false) noexcept {
        try {
            std::string response;
            response.reserve(512 + body.size());

            response += "HTTP/1.1 ";
            response += std::to_string(status);
            response += ' ';
            response += reason_phrase(status);
            response += "\r\n";
            response += "Content-Type: application/json; charset=utf-8\r\n";
            response += "Content-Length: ";
            response += std::to_string(body.size());
            response += "\r\n";
            response += "Connection: close\r\n";
            response += "Cache-Control: no-store\r\n";
            response += "X-Content-Type-Options: nosniff\r\n";

            if (!origin.empty()) {
                response += "Access-Control-Allow-Origin: ";
                response.append(origin);
                response += "\r\n";
                response += "Vary: Origin\r\n";
            }

            if (preflight) {
                response += "Access-Control-Allow-Methods: GET, PATCH, DELETE\r\n";
                response += "Access-Control-Allow-Headers: Authorization, Content-Type\r\n";
                response += "Access-Control-Max-Age: 300\r\n";
            }

            if (unauthorized) {
                response += "WWW-Authenticate: Bearer realm=\"CrayonShell Local Service\"\r\n";
            }

            response += "\r\n";
            response += body;
            send_all(socket, response);
        } catch (...) {
            // Allocation or socket failures are confined to this connection.
        }
    }
};

HttpServer::HttpServer(RequestHandler handler, HttpServerOptions options)
    : impl_(std::make_unique<Impl>(std::move(handler), options)) {}

HttpServer::~HttpServer() = default;

HttpServerStartResult HttpServer::start() {
    return impl_->start();
}

void HttpServer::stop() noexcept {
    if (impl_) impl_->stop();
}

bool HttpServer::running() const noexcept {
    return impl_ && impl_->is_running.load();
}

std::uint16_t HttpServer::port() const noexcept {
    return impl_ ? impl_->bound_port.load() : 0;
}

std::size_t HttpServer::queued_connections() const noexcept {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->queue_mutex);
    return impl_->pending_sockets.size();
}

std::size_t HttpServer::active_connections() const noexcept {
    if (!impl_) return 0;
    std::lock_guard<std::mutex> lock(impl_->queue_mutex);
    return impl_->active_sockets.size();
}

} // namespace craybridge::http
