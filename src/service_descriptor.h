#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace craybridge::bootstrap {

bool build_service_descriptor_json(
    std::uint32_t process_id,
    std::uint16_t port,
    std::string_view bearer_token,
    std::string_view instance_id,
    std::uint64_t expires_at_unix_ms,
    std::string& output) noexcept;

class ServiceDescriptorPublisher final {
public:
    // Receives the native foobar2000 profile directory resolved by the SDK.
    // Tests pass an isolated temporary profile directory.
    explicit ServiceDescriptorPublisher(std::filesystem::path profile_root);

    ServiceDescriptorPublisher(const ServiceDescriptorPublisher&) = delete;
    ServiceDescriptorPublisher& operator=(const ServiceDescriptorPublisher&) = delete;

    bool publish(
        std::uint16_t port,
        std::string_view bearer_token,
        unsigned long& error_code) noexcept;

    // Deletes the published file only when it still contains this instance ID.
    void remove_if_owned() noexcept;

    [[nodiscard]] std::string_view instance_id() const noexcept;
    [[nodiscard]] const std::filesystem::path& descriptor_path() const noexcept;

private:
    std::filesystem::path profile_root_;
    std::filesystem::path descriptor_path_;
    std::string instance_id_;
};

} // namespace craybridge::bootstrap
