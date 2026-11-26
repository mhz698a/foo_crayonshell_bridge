#include "service_descriptor.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace craybridge::bootstrap {
namespace {

constexpr wchar_t kFoobarDirectory[] = L"foobar2000";
constexpr wchar_t kComponentDirectory[] = L"foo_crayonshell_bridge";
constexpr wchar_t kDescriptorFile[] = L"service.json";
constexpr std::uint64_t kDescriptorLifetimeMs = 24ULL * 60ULL * 60ULL * 1000ULL;
constexpr std::uint64_t kWindowsToUnixEpochTicks = 116444736000000000ULL;
constexpr DWORD kMaximumDescriptorBytes = 8192;

bool is_base64url_256(std::string_view value) noexcept {
    if (value.size() != 43) return false;
    for (const unsigned char ch : value) {
        const bool valid =
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_';
        if (!valid) return false;
    }
    return true;
}

std::string base64url_encode(const unsigned char* bytes, std::size_t size) {
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
        if (remaining > 1) encoded.push_back(alphabet[(value >> 6) & 0x3f]);
        if (remaining > 2) encoded.push_back(alphabet[value & 0x3f]);
    }

    return encoded;
}

bool generate_random_id(std::string& value, unsigned long& error_code) noexcept {
    std::array<unsigned char, 32> bytes{};
    const NTSTATUS status = ::BCryptGenRandom(
        nullptr,
        bytes.data(),
        static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    if (status != 0) {
        error_code = static_cast<unsigned long>(status);
        return false;
    }

    try {
        value = base64url_encode(bytes.data(), bytes.size());
    } catch (...) {
        error_code = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }

    error_code = ERROR_SUCCESS;
    return true;
}

std::uint64_t current_unix_time_ms() noexcept {
    FILETIME now{};
    ::GetSystemTimeAsFileTime(&now);

    ULARGE_INTEGER ticks{};
    ticks.LowPart = now.dwLowDateTime;
    ticks.HighPart = now.dwHighDateTime;

    if (ticks.QuadPart < kWindowsToUnixEpochTicks) return 0;
    return (ticks.QuadPart - kWindowsToUnixEpochTicks) / 10000ULL;
}

bool get_local_app_data(std::filesystem::path& output, unsigned long& error_code) {
    const DWORD required = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required == 0 || required > 32767) {
        error_code = ::GetLastError();
        if (error_code == ERROR_SUCCESS) error_code = ERROR_ENVVAR_NOT_FOUND;
        return false;
    }

    std::vector<wchar_t> buffer(required);
    const DWORD written = ::GetEnvironmentVariableW(
        L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) {
        error_code = ::GetLastError();
        if (error_code == ERROR_SUCCESS) error_code = ERROR_INVALID_DATA;
        return false;
    }

    try {
        output = std::filesystem::path(buffer.data());
    } catch (...) {
        error_code = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }

    error_code = ERROR_SUCCESS;
    return true;
}

bool build_user_security_descriptor(
    PSECURITY_DESCRIPTOR& descriptor,
    unsigned long& error_code) {
    descriptor = nullptr;

    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        error_code = ::GetLastError();
        return false;
    }

    DWORD required = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    if (required == 0) {
        error_code = ::GetLastError();
        ::CloseHandle(token);
        return false;
    }

    std::vector<std::byte> token_buffer;
    try {
        token_buffer.resize(required);
    } catch (...) {
        error_code = ERROR_NOT_ENOUGH_MEMORY;
        ::CloseHandle(token);
        return false;
    }

    if (!::GetTokenInformation(
            token, TokenUser, token_buffer.data(), required, &required)) {
        error_code = ::GetLastError();
        ::CloseHandle(token);
        return false;
    }
    ::CloseHandle(token);

    const auto* token_user =
        reinterpret_cast<const TOKEN_USER*>(token_buffer.data());

    LPWSTR sid_text = nullptr;
    if (!::ConvertSidToStringSidW(token_user->User.Sid, &sid_text)) {
        error_code = ::GetLastError();
        return false;
    }

    std::wstring sddl;
    try {
        // Protected DACL: only the current user and LocalSystem receive access.
        sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;";
        sddl += sid_text;
        sddl += L")";
    } catch (...) {
        ::LocalFree(sid_text);
        error_code = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    ::LocalFree(sid_text);

    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
        error_code = ::GetLastError();
        return false;
    }

    error_code = ERROR_SUCCESS;
    return true;
}

bool is_directory_without_reparse_point(const std::filesystem::path& path) noexcept {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

bool ensure_parent_directory(const std::filesystem::path& path,
                             unsigned long& error_code) {
    if (::CreateDirectoryW(path.c_str(), nullptr)) {
        error_code = ERROR_SUCCESS;
        return true;
    }

    error_code = ::GetLastError();
    if (error_code != ERROR_ALREADY_EXISTS) return false;
    if (!is_directory_without_reparse_point(path)) {
        error_code = ERROR_CANT_ACCESS_FILE;
        return false;
    }

    error_code = ERROR_SUCCESS;
    return true;
}

bool ensure_protected_directory(const std::filesystem::path& path,
                                PSECURITY_DESCRIPTOR security_descriptor,
                                unsigned long& error_code) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = security_descriptor;
    attributes.bInheritHandle = FALSE;

    if (!::CreateDirectoryW(path.c_str(), &attributes)) {
        error_code = ::GetLastError();
        if (error_code != ERROR_ALREADY_EXISTS) return false;
        if (!is_directory_without_reparse_point(path)) {
            error_code = ERROR_CANT_ACCESS_FILE;
            return false;
        }
    }

    if (!::SetFileSecurityW(
            path.c_str(),
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            security_descriptor)) {
        error_code = ::GetLastError();
        return false;
    }

    error_code = ERROR_SUCCESS;
    return true;
}

bool write_all(HANDLE file, std::string_view bytes, unsigned long& error_code) noexcept {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
        const DWORD chunk = static_cast<DWORD>(
            (remaining > static_cast<std::size_t>((std::numeric_limits<DWORD>::max)()))
                ? (std::numeric_limits<DWORD>::max)()
                : remaining);
        DWORD written = 0;
        if (!::WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) ||
            written == 0) {
            error_code = ::GetLastError();
            if (error_code == ERROR_SUCCESS) error_code = ERROR_WRITE_FAULT;
            return false;
        }
        offset += written;
    }

    if (!::FlushFileBuffers(file)) {
        error_code = ::GetLastError();
        return false;
    }

    error_code = ERROR_SUCCESS;
    return true;
}

bool read_descriptor_file(const std::filesystem::path& path,
                          std::string& content) noexcept {
    HANDLE file = ::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER size{};
    const bool safe_file =
        ::GetFileInformationByHandle(file, &info) &&
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
        ::GetFileSizeEx(file, &size) &&
        size.QuadPart >= 0 &&
        size.QuadPart <= static_cast<LONGLONG>(kMaximumDescriptorBytes);

    if (!safe_file) {
        ::CloseHandle(file);
        return false;
    }

    try {
        content.resize(static_cast<std::size_t>(size.QuadPart));
    } catch (...) {
        ::CloseHandle(file);
        return false;
    }

    std::size_t offset = 0;
    while (offset < content.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(content.size() - offset,
                       static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        if (!::ReadFile(file, content.data() + offset, chunk, &read, nullptr) || read == 0) {
            content.clear();
            ::CloseHandle(file);
            return false;
        }
        offset += read;
    }

    ::CloseHandle(file);
    return true;
}

} // namespace

bool build_service_descriptor_json(
    std::uint32_t process_id,
    std::uint16_t port,
    std::string_view bearer_token,
    std::string_view instance_id,
    std::uint64_t expires_at_unix_ms,
    std::string& output) noexcept {
    if (process_id == 0 || port == 0 ||
        !is_base64url_256(bearer_token) ||
        !is_base64url_256(instance_id) ||
        expires_at_unix_ms == 0) {
        return false;
    }

    try {
        output =
            "{\"success\":true,"
            "\"service\":\"crayonshell\","
            "\"protocolVersion\":1,"
            "\"pid\":" + std::to_string(process_id) + ","
            "\"baseUrl\":\"http://127.0.0.1:" + std::to_string(port) + "\","
            "\"token\":\"" + std::string(bearer_token) + "\","
            "\"instanceId\":\"" + std::string(instance_id) + "\","
            "\"expiresAt\":" + std::to_string(expires_at_unix_ms) + "}";
    } catch (...) {
        output.clear();
        return false;
    }

    return true;
}

ServiceDescriptorPublisher::ServiceDescriptorPublisher(
    std::filesystem::path local_app_data_override)
    : local_app_data_override_(std::move(local_app_data_override)) {}

bool ServiceDescriptorPublisher::publish(
    std::uint16_t port,
    std::string_view bearer_token,
    unsigned long& error_code) noexcept {
    if (port == 0 || !is_base64url_256(bearer_token)) {
        error_code = ERROR_INVALID_PARAMETER;
        return false;
    }

    PSECURITY_DESCRIPTOR security_descriptor = nullptr;
    if (!build_user_security_descriptor(security_descriptor, error_code)) return false;

    const auto free_security_descriptor = [&security_descriptor] {
        if (security_descriptor) {
            ::LocalFree(security_descriptor);
            security_descriptor = nullptr;
        }
    };

    try {
        std::filesystem::path local_app_data = local_app_data_override_;
        if (local_app_data.empty() &&
            !get_local_app_data(local_app_data, error_code)) {
            free_security_descriptor();
            return false;
        }

        const auto root = local_app_data / kFoobarDirectory;
        const auto component_directory = root / kComponentDirectory;
        const auto process_directory =
            component_directory / std::to_wstring(::GetCurrentProcessId());

        if (!ensure_parent_directory(root, error_code) ||
            !ensure_protected_directory(component_directory, security_descriptor, error_code) ||
            !ensure_protected_directory(process_directory, security_descriptor, error_code)) {
            free_security_descriptor();
            return false;
        }

        const std::uint64_t now = current_unix_time_ms();
        if (now == 0 || now > (std::numeric_limits<std::uint64_t>::max)() - kDescriptorLifetimeMs) {
            free_security_descriptor();
            error_code = ERROR_INVALID_TIME;
            return false;
        }
        const std::uint64_t expires_at = now + kDescriptorLifetimeMs;

        if (instance_id_.empty()) {
            unsigned long random_error = ERROR_SUCCESS;
            if (!generate_random_id(instance_id_, random_error)) {
                free_security_descriptor();
                error_code = random_error;
                return false;
            }
        }

        std::string document;
        if (!build_service_descriptor_json(
                ::GetCurrentProcessId(), port, bearer_token,
                instance_id_, expires_at, document)) {
            free_security_descriptor();
            error_code = ERROR_INVALID_DATA;
            return false;
        }

        descriptor_path_ = process_directory / kDescriptorFile;
        std::wstring temporary_suffix = L".";
        temporary_suffix += std::wstring(instance_id_.begin(), instance_id_.end());
        temporary_suffix += L".tmp";
        const std::filesystem::path temporary_path =
            std::filesystem::path(descriptor_path_.native() + temporary_suffix);

        // Refuse a planted reparse point at the final descriptor name.
        const DWORD existing_attributes = ::GetFileAttributesW(descriptor_path_.c_str());
        if (existing_attributes != INVALID_FILE_ATTRIBUTES &&
            (existing_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            free_security_descriptor();
            error_code = ERROR_CANT_ACCESS_FILE;
            return false;
        }

        SECURITY_ATTRIBUTES file_attributes{};
        file_attributes.nLength = sizeof(file_attributes);
        file_attributes.lpSecurityDescriptor = security_descriptor;
        file_attributes.bInheritHandle = FALSE;

        // Clean up an instance-specific temporary file left by an earlier failed publish.
        ::DeleteFileW(temporary_path.c_str());

        HANDLE file = ::CreateFileW(
            temporary_path.c_str(),
            GENERIC_WRITE,
            0,
            &file_attributes,
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            free_security_descriptor();
            error_code = ::GetLastError();
            return false;
        }

        const bool written = write_all(file, document, error_code);
        ::CloseHandle(file);
        if (!written) {
            ::DeleteFileW(temporary_path.c_str());
            free_security_descriptor();
            return false;
        }

        if (!::MoveFileExW(
                temporary_path.c_str(),
                descriptor_path_.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error_code = ::GetLastError();
            ::DeleteFileW(temporary_path.c_str());
            free_security_descriptor();
            return false;
        }

        free_security_descriptor();
        error_code = ERROR_SUCCESS;
        return true;
    } catch (...) {
        free_security_descriptor();
        error_code = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
}

void ServiceDescriptorPublisher::remove_if_owned() noexcept {
    if (descriptor_path_.empty() || instance_id_.empty()) return;

    std::string content;
    if (read_descriptor_file(descriptor_path_, content)) {
        const std::string marker =
            "\"instanceId\":\"" + instance_id_ + "\"";
        const std::string pid_marker =
            "\"pid\":" + std::to_string(::GetCurrentProcessId()) + ",";
        if (content.find(marker) != std::string::npos &&
            content.find(pid_marker) != std::string::npos) {
            ::DeleteFileW(descriptor_path_.c_str());
        }
    }

    descriptor_path_.clear();
    instance_id_.clear();
}

std::string_view ServiceDescriptorPublisher::instance_id() const noexcept {
    return instance_id_;
}

const std::filesystem::path& ServiceDescriptorPublisher::descriptor_path() const noexcept {
    return descriptor_path_;
}

} // namespace craybridge::bootstrap
