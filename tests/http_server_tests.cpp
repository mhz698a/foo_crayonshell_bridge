#include "../src/http_server.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
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

class WinsockScope final {
public:
    WinsockScope() {
        valid_ = ::WSAStartup(MAKEWORD(2, 2), &data_) == 0;
    }

    ~WinsockScope() {
        if (valid_) ::WSACleanup();
    }

    [[nodiscard]] bool valid() const noexcept {
        return valid_;
    }

private:
    WSADATA data_{};
    bool valid_ = false;
};

bool send_all(SOCKET socket, const std::string& request) {
    std::size_t offset = 0;
    while (offset < request.size()) {
        const int sent = ::send(
            socket,
            request.data() + offset,
            static_cast<int>(request.size() - offset),
            0);

        if (sent == SOCKET_ERROR || sent == 0) return false;
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

std::string send_request(std::uint16_t port,
                         const std::string& request) {
    WinsockScope winsock;
    if (!winsock.valid()) return {};

    SOCKET client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) return {};

    DWORD timeout_ms = 3000;
    ::setsockopt(
        client,
        SOL_SOCKET,
        SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout_ms),
        sizeof(timeout_ms));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);

    if (::connect(
            client,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == SOCKET_ERROR) {
        ::closesocket(client);
        return {};
    }

    if (!send_all(client, request)) {
        ::shutdown(client, SD_BOTH);
        ::closesocket(client);
        return {};
    }

    std::string response;
    char buffer[2048]{};

    for (;;) {
        const int received = ::recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) break;

        try {
            response.append(buffer, static_cast<std::size_t>(received));
        } catch (...) {
            break;
        }
    }

    ::shutdown(client, SD_BOTH);
    ::closesocket(client);
    return response;
}

std::string make_request(std::uint16_t port,
                         const std::string& token,
                         const std::string& origin =
                             "https://foo-ui-webview2.local",
                         const std::string& host_override = {}) {
    const std::string host = host_override.empty()
        ? "127.0.0.1:" + std::to_string(port)
        : host_override;

    return
        "GET /api/v1/system/health HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Origin: " + origin + "\r\n"
        "Authorization: Bearer " + token + "\r\n"
        "Connection: close\r\n"
        "\r\n";
}

void test_invalid_server_configuration() {
    craybridge::http::HttpServerOptions options{};
    options.worker_count = 0;

    craybridge::http::HttpServer server(
        [](const craybridge::http::Request&) {
            return craybridge::http::HttpResponse{200, "{}"};
        },
        options);

    const auto result = server.start();
    expect(!result.success,
           "zero-worker server configuration must be rejected");
    expect(result.error_code == ERROR_INVALID_PARAMETER,
           "invalid server configuration must report ERROR_INVALID_PARAMETER");
    expect(!server.running(),
           "server with invalid configuration must remain stopped");
}

void test_loopback_server_security_and_lifecycle() {
    std::atomic<int> handler_calls{0};
    std::atomic<bool> handler_saw_get{false};
    std::atomic<bool> handler_saw_expected_target{false};

    craybridge::http::HttpServer server(
        [&handler_calls, &handler_saw_get, &handler_saw_expected_target](
            const craybridge::http::Request& request) {
            ++handler_calls;
            handler_saw_get.store(request.method == "GET");
            handler_saw_expected_target.store(
                request.target == "/api/v1/system/health");

            return craybridge::http::HttpResponse{
                200,
                "{\"success\":true,\"data\":{\"transport\":\"loopback\"}}"
            };
        });

    const auto started = server.start();
    expect(started.success, "Winsock HTTP server must start");
    if (!started.success) return;

    expect(started.port != 0, "server must select an ephemeral TCP port");
    expect(server.port() == started.port,
           "server must expose its bound ephemeral port");
    expect(server.running(), "server must report running after startup");
    expect(started.bearer_token.size() == 43,
           "32-byte CNG token must be encoded as a 43-character base64url value");

    const auto valid_response = send_request(
        started.port,
        make_request(started.port, started.bearer_token));

    expect(valid_response.find("HTTP/1.1 200 OK") == 0,
           "authenticated loopback request must receive HTTP 200");
    expect(valid_response.find("\"transport\":\"loopback\"") !=
               std::string::npos,
           "handler response body must reach the client");
    expect(valid_response.find("Access-Control-Allow-Origin: "
                               "https://foo-ui-webview2.local") !=
               std::string::npos,
           "trusted origin must be echoed in the CORS response");
    expect(valid_response.find("Connection: close") != std::string::npos,
           "responses must close the connection");
    expect(valid_response.find(started.bearer_token) == std::string::npos,
           "bearer token must never be echoed in the HTTP response");
    expect(handler_calls.load() == 1,
           "valid authenticated request must invoke the handler once");
    expect(handler_saw_get.load(),
           "handler must receive the parsed request method");
    expect(handler_saw_expected_target.load(),
           "handler must receive the parsed request target");

    const auto unauthorized = send_request(
        started.port,
        make_request(started.port, "incorrect-token"));
    expect(unauthorized.find("HTTP/1.1 401 Unauthorized") == 0,
           "invalid bearer token must receive HTTP 401");
    expect(handler_calls.load() == 1,
           "unauthorized request must not reach the route handler");

    const auto disallowed_origin = send_request(
        started.port,
        make_request(started.port, started.bearer_token,
                     "https://untrusted.example"));
    expect(disallowed_origin.find("HTTP/1.1 403 Forbidden") == 0,
           "untrusted browser origin must receive HTTP 403");
    expect(handler_calls.load() == 1,
           "untrusted origin must not reach the route handler");

    const auto invalid_host = send_request(
        started.port,
        make_request(started.port, started.bearer_token,
                     "https://foo-ui-webview2.local",
                     "127.0.0.1:1"));
    expect(invalid_host.find("HTTP/1.1 421 Misdirected Request") == 0,
           "unexpected Host header must be rejected");

    const std::string preflight =
        "OPTIONS /api/v1/system/health HTTP/1.1\r\n"
        "Host: 127.0.0.1:" + std::to_string(started.port) + "\r\n"
        "Origin: https://foo-ui-webview2.local\r\n"
        "Access-Control-Request-Method: PATCH\r\n"
        "Access-Control-Request-Headers: authorization, content-type\r\n"
        "\r\n";

    const auto preflight_response = send_request(started.port, preflight);
    expect(preflight_response.find("HTTP/1.1 204 No Content") == 0,
           "allowed CORS preflight must receive HTTP 204");
    expect(preflight_response.find(
               "Access-Control-Allow-Methods: GET, PATCH, DELETE") !=
               std::string::npos,
           "preflight must advertise only the API methods");
    expect(preflight_response.find(
               "Access-Control-Allow-Headers: Authorization, Content-Type") !=
               std::string::npos,
           "preflight must advertise only the required request headers");
    expect(preflight_response.find(
               "Access-Control-Allow-Private-Network") == std::string::npos,
           "Private Network Access must remain disabled until WebView2 runtime verification");

    const std::string invalid_preflight =
        "OPTIONS /api/v1/system/health HTTP/1.1\r\n"
        "Host: 127.0.0.1:" + std::to_string(started.port) + "\r\n"
        "Origin: https://foo-ui-webview2.local\r\n"
        "Access-Control-Request-Method: GET\r\n"
        "Access-Control-Request-Headers: x-unapproved-header\r\n"
        "\r\n";

    const auto rejected_preflight =
        send_request(started.port, invalid_preflight);
    expect(rejected_preflight.find("HTTP/1.1 403 Forbidden") == 0,
           "preflight requesting an unapproved header must be rejected");

    server.stop();
    expect(!server.running(), "stop must mark the service as not running");
    expect(server.port() == 0, "stop must clear the active service port");

    const auto restarted = server.start();
    expect(restarted.success, "server must support a clean restart");
    if (restarted.success) {
        expect(restarted.bearer_token != started.bearer_token,
               "server restart must rotate the per-instance bearer token");
        server.stop();
    }
}

 
void test_bounded_queue_returns_service_busy() {
    std::mutex gate_mutex;
    std::condition_variable gate_condition;
    bool handler_entered = false;
    bool release_handler = false;

    craybridge::http::HttpServerOptions options{};
    options.worker_count = 1;
    options.queue_capacity = 1;
    options.socket_timeout_ms = 2000;

    craybridge::http::HttpServer server(
        [&gate_mutex, &gate_condition, &handler_entered, &release_handler](
            const craybridge::http::Request&) {
            std::unique_lock<std::mutex> lock(gate_mutex);
            handler_entered = true;
            gate_condition.notify_all();
            gate_condition.wait_for(
                lock,
                std::chrono::seconds(4),
                [&release_handler] { return release_handler; });
            return craybridge::http::HttpResponse{200, "{\\"success\\":true}"};
        },
        options);

    const auto started = server.start();
    expect(started.success, "bounded queue test server must start");
    if (!started.success) return;

    std::string first_response;
    std::thread first_client([&] {
        first_response = send_request(
            started.port,
            make_request(started.port, started.bearer_token));
    });

    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        entered = gate_condition.wait_for(
            lock,
            std::chrono::seconds(2),
            [&handler_entered] { return handler_entered; });
    }
    expect(entered, "first request must occupy the only worker");

    std::string second_response;
    std::thread second_client([&] {
        second_response = send_request(
            started.port,
            make_request(started.port, started.bearer_token));
    });

    const auto queue_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (server.queued_connections() != 1 &&
           std::chrono::steady_clock::now() < queue_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(server.queued_connections() == 1,
           "second request must occupy the single pending queue slot");

    const auto overloaded = send_request(
        started.port,
        make_request(started.port, started.bearer_token));
    expect(overloaded.find("HTTP/1.1 503 Service Unavailable") == 0,
           "request beyond worker and queue capacity must receive HTTP 503");

    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release_handler = true;
    }
    gate_condition.notify_all();

    first_client.join();
    second_client.join();

    expect(first_response.find("HTTP/1.1 200 OK") == 0,
           "active request must complete after the handler is released");
    expect(second_response.find("HTTP/1.1 200 OK") == 0,
           "queued request must run after a worker becomes available");
    expect(server.queued_connections() == 0,
           "pending queue must drain after both requests complete");

    server.stop();
}

void test_slow_client_has_an_absolute_request_deadline() {
    craybridge::http::HttpServerOptions options{};
    options.worker_count = 1;
    options.queue_capacity = 1;
    options.socket_timeout_ms = 300;

    craybridge::http::HttpServer server(
        [](const craybridge::http::Request&) {
            return craybridge::http::HttpResponse{200, "{}"};
        },
        options);

    const auto started = server.start();
    expect(started.success, "slow-client test server must start");
    if (!started.success) return;

    WinsockScope winsock;
    expect(winsock.valid(), "Winsock client initialization must succeed");
    if (!winsock.valid()) {
        server.stop();
        return;
    }

    SOCKET client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    expect(client != INVALID_SOCKET, "slow-client socket must be created");
    if (client == INVALID_SOCKET) {
        server.stop();
        return;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(started.port);

    const int connected = ::connect(
        client,
        reinterpret_cast<const sockaddr*>(&address),
        sizeof(address));
    expect(connected != SOCKET_ERROR, "slow client must connect to loopback");

    const std::string first_part =
        "GET /api/v1/system/health HTTP/1.1\r\n";
    const std::string second_part =
        "Host: 127.0.0.1:" + std::to_string(started.port) + "\r\n";

    expect(send_all(client, first_part),
           "slow client must send its first request fragment");

    const auto active_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (server.active_connections() == 0 &&
           std::chrono::steady_clock::now() < active_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    expect(server.active_connections() == 1,
           "slow request must be owned by the worker");

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ::send(client, second_part.data(),
           static_cast<int>(second_part.size()), 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::string response;
    char buffer[1024]{};
    for (;;) {
        const int received = ::recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        response.append(buffer, static_cast<std::size_t>(received));
    }

    expect(response.find("HTTP/1.1 408 Request Timeout") == 0,
           "slow client must be rejected at the total request deadline");

    ::shutdown(client, SD_BOTH);
    ::closesocket(client);
    server.stop();
}

void test_vite_origin_requires_explicit_enablement() {
    craybridge::http::HttpServer default_server(
        [](const craybridge::http::Request&) {
            return craybridge::http::HttpResponse{200, "{}"};
        });

    const auto default_start = default_server.start();
    expect(default_start.success,
           "server with production-only origin policy must start");
    if (!default_start.success) return;

    const auto denied = send_request(
        default_start.port,
        make_request(default_start.port, default_start.bearer_token,
                     "http://localhost:5173"));
    expect(denied.find("HTTP/1.1 403 Forbidden") == 0,
           "Vite origin must be denied unless development mode is enabled");
    default_server.stop();

    craybridge::http::HttpServerOptions options{};
    options.allow_vite_dev_origin = true;

    craybridge::http::HttpServer dev_server(
        [](const craybridge::http::Request&) {
            return craybridge::http::HttpResponse{200, "{}"};
        },
        options);

    const auto dev_start = dev_server.start();
    expect(dev_start.success,
           "server with explicit Vite origin policy must start");
    if (!dev_start.success) return;

    const auto allowed = send_request(
        dev_start.port,
        make_request(dev_start.port, dev_start.bearer_token,
                     "http://localhost:5173"));
    expect(allowed.find("HTTP/1.1 200 OK") == 0,
           "Vite origin must work when development mode is explicitly enabled");
    dev_server.stop();
}

} // namespace

int run_http_server_tests() {
    test_invalid_server_configuration();
    test_loopback_server_security_and_lifecycle();
    test_bounded_queue_returns_service_busy();
    test_slow_client_has_an_absolute_request_deadline();
    test_vite_origin_requires_explicit_enablement();

    if (g_failures != 0) {
        std::cerr << g_failures << " HTTP server test(s) failed.\n";
        return 1;
    }

    std::cout << "All HTTP server tests passed.\n";
    return 0;
}
