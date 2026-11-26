#include "service_descriptor.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

bool require(bool condition, const char* message) {
    if (!condition) std::cerr << "[ServiceDescriptor] " << message << "\n";
    return condition;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
}

} // namespace

int run_service_descriptor_tests() {
    constexpr char token[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghi";
    constexpr char instance[] =
        "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFG";
    std::string json;

    if (!require(
            craybridge::bootstrap::build_service_descriptor_json(
                1234, 49152, token, instance, 1900000000000ULL, json),
            "valid descriptor input should serialize")) return 1;

    if (!require(json.find("\"success\":true") != std::string::npos &&
                 json.find("\"service\":\"crayonshell\"") != std::string::npos &&
                 json.find("\"protocolVersion\":1") != std::string::npos &&
                 json.find("\"pid\":1234") != std::string::npos &&
                 json.find("\"baseUrl\":\"http://127.0.0.1:49152\"") != std::string::npos &&
                 json.find(std::string("\"token\":\"") + token + "\"") != std::string::npos &&
                 json.find(std::string("\"instanceId\":\"") + instance + "\"") != std::string::npos &&
                 json.find("\"expiresAt\":1900000000000") != std::string::npos,
            "serialized descriptor must contain the versioned host contract")) return 1;

    if (!require(
            !craybridge::bootstrap::build_service_descriptor_json(
                1234, 0, token, instance, 1900000000000ULL, json),
            "zero port must be rejected")) return 1;

    if (!require(
            !craybridge::bootstrap::build_service_descriptor_json(
                1234, 49152, "not-a-token", instance, 1900000000000ULL, json),
            "malformed bearer token must be rejected")) return 1;

    if (!require(
            !craybridge::bootstrap::build_service_descriptor_json(
                0, 49152, token, instance, 1900000000000ULL, json),
            "zero process id must be rejected")) return 1;

    wchar_t temp_buffer[MAX_PATH]{};
    const DWORD temp_length = ::GetTempPathW(MAX_PATH, temp_buffer);
    if (!require(temp_length > 0 && temp_length < MAX_PATH,
                 "temporary directory must be available")) return 1;

    const auto test_root = std::filesystem::path(temp_buffer) /
        L"CrayonShellBridgeDescriptorTests" /
        std::to_wstring(::GetCurrentProcessId());
    std::error_code filesystem_error;
    std::filesystem::remove_all(test_root, filesystem_error);
    filesystem_error.clear();
    std::filesystem::create_directories(test_root, filesystem_error);
    if (!require(!filesystem_error, "test LocalAppData root must be created")) return 1;

    craybridge::bootstrap::ServiceDescriptorPublisher publisher(test_root);
    unsigned long error_code = ERROR_SUCCESS;
    if (!require(publisher.publish(49153, token, error_code),
                 "publisher must atomically publish a descriptor")) {
        std::filesystem::remove_all(test_root, filesystem_error);
        return 1;
    }

    const auto descriptor_path = publisher.descriptor_path();
    const auto published = read_file(descriptor_path);
    if (!require(!published.empty() &&
                 published.find("\"service\":\"crayonshell\"") != std::string::npos &&
                 published.find("\"pid\":" + std::to_string(::GetCurrentProcessId())) != std::string::npos &&
                 published.find("\"baseUrl\":\"http://127.0.0.1:49153\"") != std::string::npos &&
                 !publisher.instance_id().empty(),
                 "published descriptor must identify the current process and loopback service")) {
        publisher.remove_if_owned();
        std::filesystem::remove_all(test_root, filesystem_error);
        return 1;
    }

    publisher.remove_if_owned();
    if (!require(::GetFileAttributesW(descriptor_path.c_str()) == INVALID_FILE_ATTRIBUTES,
                 "shutdown cleanup must remove the descriptor owned by this instance")) {
        std::filesystem::remove_all(test_root, filesystem_error);
        return 1;
    }

    std::filesystem::remove_all(test_root, filesystem_error);
    std::cout << "Service descriptor tests passed.\n";
    return 0;
}
