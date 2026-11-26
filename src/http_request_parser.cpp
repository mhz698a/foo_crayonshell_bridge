#include "http_request_parser.h"

namespace craybridge::http {
namespace {

bool append_bounded(std::string& destination,
                    const char* data,
                    std::size_t size,
                    std::size_t maximum) noexcept {
    if (size > maximum - destination.size()) {
        return false;
    }

    try {
        destination.append(data, size);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

RequestParser::RequestParser() {
    llhttp_settings_init(&settings_);
    settings_.on_method = &RequestParser::on_method;
    settings_.on_url = &RequestParser::on_url;
    settings_.on_header_field = &RequestParser::on_header_field;
    settings_.on_header_value = &RequestParser::on_header_value;
    settings_.on_header_value_complete =
        &RequestParser::on_header_value_complete;
    settings_.on_headers_complete = &RequestParser::on_headers_complete;
    settings_.on_body = &RequestParser::on_body;
    settings_.on_message_complete = &RequestParser::on_message_complete;

    llhttp_init(&parser_, HTTP_REQUEST, &settings_);
    parser_.data = this;
}

RequestParser* RequestParser::from(llhttp_t* parser) noexcept {
    if (parser == nullptr) {
        return nullptr;
    }
    return static_cast<RequestParser*>(parser->data);
}

int RequestParser::fail(llhttp_t* parser, const char* reason) noexcept {
    if (auto* self = from(parser)) {
        self->failed_ = true;
        self->error_message_ = reason;
    }

    if (parser != nullptr) {
        llhttp_set_error_reason(parser, reason);
    }
    return HPE_USER;
}

int RequestParser::append_method(const char* data,
                                 std::size_t size) noexcept {
    if (!append_bounded(request_.method, data, size,
                        kMaxRequestMethodBytes)) {
        return fail(&parser_, "request method exceeds the configured limit");
    }
    header_bytes_ += size;
    if (header_bytes_ > kMaxRequestHeaderBytes) {
        return fail(&parser_, "request headers exceed the configured limit");
    }
    return HPE_OK;
}

int RequestParser::append_target(const char* data,
                                 std::size_t size) noexcept {
    if (!append_bounded(request_.target, data, size,
                        kMaxRequestTargetBytes)) {
        return fail(&parser_, "request target exceeds the configured limit");
    }
    header_bytes_ += size;
    if (header_bytes_ > kMaxRequestHeaderBytes) {
        return fail(&parser_, "request headers exceed the configured limit");
    }
    return HPE_OK;
}

int RequestParser::append_header_field(const char* data,
                                       std::size_t size) noexcept {
    if (!append_bounded(current_header_field_, data, size,
                        kMaxRequestHeaderBytes)) {
        return fail(&parser_, "request headers exceed the configured limit");
    }
    header_bytes_ += size;
    if (header_bytes_ > kMaxRequestHeaderBytes) {
        return fail(&parser_, "request headers exceed the configured limit");
    }
    return HPE_OK;
}

int RequestParser::append_header_value(const char* data,
                                       std::size_t size) noexcept {
    if (!append_bounded(current_header_value_, data, size,
                        kMaxRequestHeaderBytes)) {
        return fail(&parser_, "request headers exceed the configured limit");
    }
    header_bytes_ += size;
    if (header_bytes_ > kMaxRequestHeaderBytes) {
        return fail(&parser_, "request headers exceed the configured limit");
    }
    return HPE_OK;
}

int RequestParser::append_body(const char* data,
                               std::size_t size) noexcept {
    if (!append_bounded(request_.body, data, size,
                        kMaxRequestBodyBytes)) {
        return fail(&parser_, "request body exceeds the configured limit");
    }
    return HPE_OK;
}

int RequestParser::on_method(llhttp_t* parser,
                             const char* data,
                             std::size_t size) noexcept {
    auto* self = from(parser);
    return self == nullptr
        ? HPE_USER
        : self->append_method(data, size);
}

int RequestParser::on_url(llhttp_t* parser,
                          const char* data,
                          std::size_t size) noexcept {
    auto* self = from(parser);
    return self == nullptr
        ? HPE_USER
        : self->append_target(data, size);
}

int RequestParser::on_header_field(llhttp_t* parser,
                                   const char* data,
                                   std::size_t size) noexcept {
    auto* self = from(parser);
    return self == nullptr
        ? HPE_USER
        : self->append_header_field(data, size);
}

int RequestParser::on_header_value(llhttp_t* parser,
                                   const char* data,
                                   std::size_t size) noexcept {
    auto* self = from(parser);
    return self == nullptr
        ? HPE_USER
        : self->append_header_value(data, size);
}

int RequestParser::on_header_value_complete(llhttp_t* parser) noexcept {
    auto* self = from(parser);
    if (self == nullptr) {
        return HPE_USER;
    }

    if (self->current_header_field_.empty()) {
        return self->fail(parser, "request contains an invalid header");
    }

    if (self->request_.headers.size() >= kMaxRequestHeaderCount) {
        return self->fail(parser, "request contains too many headers");
    }

    try {
        self->request_.headers.push_back({
            std::move(self->current_header_field_),
            std::move(self->current_header_value_)
        });
        self->current_header_field_.clear();
        self->current_header_value_.clear();
    } catch (...) {
        return self->fail(parser, "unable to store request headers");
    }

    return HPE_OK;
}

int RequestParser::on_headers_complete(llhttp_t* parser) noexcept {
    auto* self = from(parser);
    if (self == nullptr) {
        return HPE_USER;
    }

    self->request_.http_major = llhttp_get_http_major(parser);
    self->request_.http_minor = llhttp_get_http_minor(parser);

    if (self->request_.http_major != 1 ||
        self->request_.http_minor != 1) {
        return self->fail(parser, "only HTTP/1.1 requests are accepted");
    }

    if (llhttp_get_upgrade(parser) != 0 ||
        self->request_.method == "CONNECT") {
        return self->fail(parser, "protocol upgrades and CONNECT are not supported");
    }

    return HPE_OK;
}

int RequestParser::on_body(llhttp_t* parser,
                           const char* data,
                           std::size_t size) noexcept {
    auto* self = from(parser);
    return self == nullptr
        ? HPE_USER
        : self->append_body(data, size);
}

int RequestParser::on_message_complete(llhttp_t* parser) noexcept {
    auto* self = from(parser);
    if (self == nullptr) {
        return HPE_USER;
    }

    self->complete_ = true;

    // The server uses one request per connection. Pause at the first complete
    // message so pipelined bytes can never be interpreted as a second request.
    return HPE_PAUSED;
}

ParseResult RequestParser::execute(const char* data,
                                   std::size_t size) noexcept {
    if (failed_) {
        return {ParseState::Error, error_message_};
    }
    if (complete_) {
        return {ParseState::Error, "request parser already completed"};
    }
    if (size != 0 && data == nullptr) {
        failed_ = true;
        error_message_ = "request input buffer is null";
        return {ParseState::Error, error_message_};
    }
    if (size > kMaxRequestBytes - received_bytes_) {
        failed_ = true;
        error_message_ = "request exceeds the configured total size limit";
        return {ParseState::Error, error_message_};
    }

    received_bytes_ += size;
    const auto error = llhttp_execute(&parser_, data, size);

    if (complete_ && error == HPE_PAUSED) {
        return {ParseState::Complete, nullptr};
    }
    if (error == HPE_OK) {
        return {ParseState::NeedMoreData, nullptr};
    }

    failed_ = true;
    if (error_message_ == nullptr) {
        error_message_ = llhttp_errno_name(error);
    }
    return {ParseState::Error, error_message_};
}

ParseResult RequestParser::finish() noexcept {
    if (failed_) {
        return {ParseState::Error, error_message_};
    }
    if (complete_) {
        return {ParseState::Complete, nullptr};
    }

    const auto error = llhttp_finish(&parser_);
    if (complete_) {
        return {ParseState::Complete, nullptr};
    }

    failed_ = true;
    if (error_message_ == nullptr) {
        error_message_ = llhttp_errno_name(error);
    }
    return {ParseState::Error, error_message_};
}

} // namespace craybridge::http
