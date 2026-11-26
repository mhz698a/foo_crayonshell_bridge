#include <SDK/foobar2000.h>

#include "api_router.h"
#include "http_server.h"
#include "service_descriptor.h"
#include "version.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <string>

DECLARE_COMPONENT_VERSION(
    "CrayonShell File Properties Bridge",
    CRAYONSHELL_BRIDGE_VERSION,
    "Loopback-only local file metadata API for CrayonShell."
);

VALIDATE_COMPONENT_FILENAME("foo_crayonshell_bridge.dll");

namespace craybridge {
namespace {

std::filesystem::path get_profile_root_path() {
    pfc::string8 profile_path;
    filesystem::g_get_display_path(core_api::get_profile_path(), profile_path);

    const char* utf8_path = profile_path.get_ptr();
    if (!utf8_path || *utf8_path == '\0') return {};

    std::string native_path(utf8_path);
    if (native_path.rfind("file://", 0) == 0) {
        native_path.erase(0, 7);
        const bool file_uri_drive_root =
            native_path.size() >= 3 &&
            native_path[0] == '/' &&
            ((native_path[1] >= 'A' && native_path[1] <= 'Z') ||
             (native_path[1] >= 'a' && native_path[1] <= 'z')) &&
            native_path[2] == ':';
        if (file_uri_drive_root) native_path.erase(0, 1);
    }
    for (char& character : native_path) {
        if (character == '/') character = '\\\\';
    }

    const auto wide_path = pfc::stringcvt::string_wide_from_utf8(native_path.c_str());
    if (!wide_path.get_ptr() || *wide_path.get_ptr() == L'\0') return {};
    return std::filesystem::path(wide_path.get_ptr());
}

} // namespace

class BridgeRuntime : public initquit {
public:
    void on_init() override {
        try {
            shutting_down_.store(false);

            // Resolve the same native profile directory exposed by misc.getProfilePath
            // before leaving foobar2000's UI-thread initialization callback.
            const auto profile_root = get_profile_root_path();
            if (profile_root.empty()) {
                OutputDebugStringA(
                    "foo_crayonshell_bridge: unable to resolve the foobar2000 profile directory.\n");
                return;
            }
            descriptor_publisher_ =
                std::make_unique<bootstrap::ServiceDescriptorPublisher>(profile_root);

            api_ = std::make_unique<api::ApiController>();
            server_ = std::make_unique<http::HttpServer>(
                [this](const http::Request& request) {
                    if (!api_ || shutting_down_.load()) {
                        return http::HttpResponse{
                            503,
                            "{\"success\":false,\"error\":{"
                            "\"code\":\"SERVICE_STOPPING\","
                            "\"message\":\"The local metadata service is stopping.\"}}"
                        };
                    }
                    return api_->handle(request);
                });

            // Job workers, the Winsock listener and descriptor publication
            // run away from foobar2000's UI-thread initialization callback.
            startup_thread_ = std::thread([this] {
                if (shutting_down_.load()) return;

                if (!api_->start()) {
                    OutputDebugStringA(
                        "foo_crayonshell_bridge: timestamp job workers failed to start.\n");
                    return;
                }

                if (shutting_down_.load()) {
                    api_->stop();
                    return;
                }

                const auto started = server_->start();
                if (!started.success) {
                    api_->stop();
                    OutputDebugStringA(
                        "foo_crayonshell_bridge: HTTP service startup failed.\n");
                    return;
                }

                // Publish only after the listener is live. The descriptor is
                // private to this Windows user and scoped to this process ID.
                unsigned long descriptor_error = ERROR_SUCCESS;
                if (!descriptor_publisher_ ||
                    !descriptor_publisher_->publish(
                        started.port, started.bearer_token, descriptor_error)) {
                    server_->stop();
                    api_->stop();
                    OutputDebugStringA(
                        "foo_crayonshell_bridge: secure service descriptor publication failed.\n");
                    return;
                }

                // Never leave an advertised, stopped service during shutdown.
                if (shutting_down_.load()) {
                    if (descriptor_publisher_) descriptor_publisher_->remove_if_owned();
                    server_->stop();
                    api_->stop();
                    return;
                }

                try {
                    descriptor_refresh_thread_ = std::thread(
                        [this, port = started.port, token = started.bearer_token] {
                            std::unique_lock lock(descriptor_refresh_mutex_);
                            while (!descriptor_refresh_condition_.wait_for(
                                lock, std::chrono::hours(6),
                                [this] { return shutting_down_.load(); })) {
                                lock.unlock();

                                unsigned long refresh_error = ERROR_SUCCESS;
                                if (!descriptor_publisher_ ||
                                    !descriptor_publisher_->publish(
                                        port, token, refresh_error)) {
                                    OutputDebugStringA(
                                        "foo_crayonshell_bridge: service descriptor refresh failed.\n");
                                }

                                lock.lock();
                            }
                        });
                } catch (...) {
                    if (descriptor_publisher_) descriptor_publisher_->remove_if_owned();
                    server_->stop();
                    api_->stop();
                    OutputDebugStringA(
                        "foo_crayonshell_bridge: descriptor refresh worker failed to start.\n");
                }
            });
        } catch (...) {
            shutting_down_.store(true);
            descriptor_refresh_condition_.notify_all();
            if (api_) api_->begin_shutdown();
            if (startup_thread_.joinable()) startup_thread_.join();
            if (descriptor_refresh_thread_.joinable()) descriptor_refresh_thread_.join();
            if (descriptor_publisher_) descriptor_publisher_->remove_if_owned();
            if (server_) server_->stop();
            if (api_) api_->stop();
            server_.reset();
            api_.reset();
        }
    }

    void on_quit() override {
        // Stop route work and descriptor refresh, then remove the descriptor
        // before stopping the HTTP listener.
        shutting_down_.store(true);
        descriptor_refresh_condition_.notify_all();
        if (api_) api_->begin_shutdown();

        if (startup_thread_.joinable()) {
            startup_thread_.join();
        }
        descriptor_refresh_condition_.notify_all();
        if (descriptor_refresh_thread_.joinable()) {
            descriptor_refresh_thread_.join();
        }

        if (descriptor_publisher_) descriptor_publisher_->remove_if_owned();
        if (server_) server_->stop();
        if (api_) api_->stop();
        server_.reset();
        api_.reset();
    }

private:
    std::atomic<bool> shutting_down_{false};
    std::unique_ptr<api::ApiController> api_;
    std::unique_ptr<http::HttpServer> server_;
    std::unique_ptr<bootstrap::ServiceDescriptorPublisher> descriptor_publisher_;
    std::thread startup_thread_;
    std::thread descriptor_refresh_thread_;
    std::mutex descriptor_refresh_mutex_;
    std::condition_variable descriptor_refresh_condition_;
};

} // namespace craybridge

static initquit_factory_t<craybridge::BridgeRuntime> g_bridge_runtime_factory;
