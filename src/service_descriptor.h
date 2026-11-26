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
    // An optional LocalAppData root override is used by the isolated test suite.
    explicit ServiceDescriptorPublisher(std::filesystem::path local_app_data_override = {});

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
    std::filesystem::path local_app_data_override_;
    std::filesystem::path descriptor_path_;
    std::string instance_id_;
};

} // namespace craybridge::bootstrap
