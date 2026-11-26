#include "../src/http_request_parser.h"

#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ++g_failures;
        std::cerr << "FAILED: " << message << '\n';
    }
}

void test_simple_request() {
    const std::string input =
        "GET /api/v1/system/health HTTP/1.1\r\n"
        "Host: 127.0.0.1:49152\r\n"
        "Authorization: Bearer test-token\r\n"
        "\r\n";

    craybridge::http::RequestParser parser;
    const auto result = parser.execute(input.data(), input.size());

    expect(result.state == craybridge::http::ParseState::Complete,
           "complete HTTP/1.1 request must parse");
    expect(parser.request().method == "GET",
           "request method must be captured");
    expect(parser.request().target == "/api/v1/system/health",
           "request target must be captured");
    expect(parser.request().http_major == 1 &&
               parser.request().http_minor == 1,
           "HTTP version must be captured");
    expect(parser.request().headers.size() == 2,
           "all request headers must be captured");
    expect(parser.request().headers[0].name == "Host",
           "header name must be preserved");
    expect(parser.request().headers[0].value == "127.0.0.1:49152",
           "header value must be preserved");
    expect(parser.request().headers[1].value == "Bearer test-token",
           "authorization value must be captured");
}

void test_incremental_request() {
    const std::string first =
        "GET /api/v1/system/health HTTP/1.1\r\nHost:";
    const std::string second =
        " 127.0.0.1\r\nAuthorization: Bearer token\r\n\r\n";

    craybridge::http::RequestParser parser;
    const auto partial = parser.execute(first.data(), first.size());
    expect(partial.state == craybridge::http::ParseState::NeedMoreData,
           "incomplete request must request more data");

    const auto complete = parser.execute(second.data(), second.size());
    expect(complete.state == craybridge::http::ParseState::Complete,
           "request split across TCP reads must complete");
    expect(parser.request().headers.size() == 2,
           "headers split across buffers must be reconstructed");
    expect(parser.request().headers[0].value == "127.0.0.1",
           "split header value must not lose leading whitespace semantics");
}

void test_content_length_body() {
    const std::string body = "{\"a\":1}";
    const std::string input =
        "PATCH /api/v1/items/0/timestamps HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 7\r\n"
        "\r\n" + body;

    craybridge::http::RequestParser parser;
    const auto result = parser.execute(input.data(), input.size());

    expect(result.state == craybridge::http::ParseState::Complete,
           "Content-Length request body must parse");
    expect(parser.request().body == body,
           "Content-Length body must be reconstructed exactly");
}

void test_chunked_body() {
    const std::string input =
        "POST /api/v1/jobs HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "4\r\nWiki\r\n"
        "5\r\npedia\r\n"
        "0\r\n\r\n";

    craybridge::http::RequestParser parser;
    const auto result = parser.execute(input.data(), input.size());

    expect(result.state == craybridge::http::ParseState::Complete,
           "valid chunked request body must parse");
    expect(parser.request().body == "Wikipedia",
           "chunk framing must not appear in the decoded body");
}

void test_conflicting_content_length_and_transfer_encoding() {
    const std::string input =
        "POST /api/v1/jobs HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Length: 4\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n";

    craybridge::http::RequestParser parser;
    const auto result = parser.execute(input.data(), input.size());

    expect(result.state == craybridge::http::ParseState::Error,
           "conflicting Content-Length and Transfer-Encoding must be rejected");
}

void test_http_10_is_rejected() {
    const std::string input =
        "GET / HTTP/1.0\r\n"
        "Host: 127.0.0.1\r\n"
        "\r\n";

    craybridge::http::RequestParser parser;
    const auto result = parser.execute(input.data(), input.size());

    expect(result.state == craybridge::http::ParseState::Error,
           "HTTP/1.0 must be rejected by the HTTP/1.1-only contract");
}

void test_upgrade_is_rejected() {
    const std::string input =
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: Upgrade\r\n"
        "Upgrade: websocket\r\n"
        "\r\n";

    craybridge::http::RequestParser parser;
    const auto result = parser.execute(input.data(), input.size());

    expect(result.state == craybridge::http::ParseState::Error,
           "protocol upgrades must be rejected");
}

void test_header_limit() {
    std::string input =
        "GET / HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "X-Large: ";
    input.append(craybridge::http::kMaxRequestHeaderBytes, 'a');
    input += "\r\n\r\n";

    craybridge::http::RequestParser parser;
    const auto result = parser.execute(input.data(), input.size());

    expect(result.state == craybridge::http::ParseState::Error,
           "oversized request headers must be rejected");
}

void test_body_limit() {
    const auto bodySize =
        craybridge::http::kMaxRequestBodyBytes + 1;

    std::string input =
        "POST /api/v1/jobs HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Length: " + std::to_string(bodySize) + "\r\n"
        "\r\n";
    input.append(bodySize, 'x');

    craybridge::http::RequestParser parser;
    const auto result = parser.execute(input.data(), input.size());

    expect(result.state == craybridge::http::ParseState::Error,
           "oversized request bodies must be rejected");
}

void test_one_request_per_connection() {
    const std::string input =
        "GET /first HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n"
        "GET /second HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";

    craybridge::http::RequestParser parser;
    const auto result = parser.execute(input.data(), input.size());

    expect(result.state == craybridge::http::ParseState::Complete,
           "first request in a connection must complete");
    expect(parser.request().target == "/first",
           "parser must stop after the first request");
    expect(parser.execute("", 0).state ==
               craybridge::http::ParseState::Error,
           "parser must not accept additional bytes after completion");
}

} // namespace

int wmain() {
    test_simple_request();
    test_incremental_request();
    test_content_length_body();
    test_chunked_body();
    test_conflicting_content_length_and_transfer_encoding();
    test_http_10_is_rejected();
    test_upgrade_is_rejected();
    test_header_limit();
    test_body_limit();
    test_one_request_per_connection();

    if (g_failures != 0) {
        std::cerr << g_failures << " HTTP parser test(s) failed.\n";
        return 1;
    }

    std::cout << "All HTTP parser tests passed.\n";
    return 0;
}
