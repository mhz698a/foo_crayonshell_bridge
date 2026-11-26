#include <SDK/foobar2000.h>

#include "api_router.h"
#include "http_server.h"
#include "version.h"

#include <atomic>
#include <memory>
#include <thread>

DECLARE_COMPONENT_VERSION(
    "CrayonShell File Properties Bridge",
    CRAYONSHELL_BRIDGE_VERSION,
    "Loopback-only local file metadata API for CrayonShell."
);

VALIDATE_COMPONENT_FILENAME("foo_crayonshell_bridge.dll");

namespace craybridge {

class BridgeRuntime : public initquit {
public:
    void on_init() override {
        try {
            shutting_down_.store(false);
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

            // Job workers and the Winsock listener are created away from
            // foobar2000's UI-thread initialization callback.
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

                // Handles a shutdown requested while start() was in progress.
                if (shutting_down_.load()) {
                    server_->stop();
                    api_->stop();
                }

                // Port/token remain private until the trusted
                // foo_ui_webview2 bootstrap is implemented and verified.
            });
        } catch (...) {
            shutting_down_.store(true);
            if (api_) api_->begin_shutdown();
            if (startup_thread_.joinable()) startup_thread_.join();
            if (server_) server_->stop();
            if (api_) api_->stop();
            server_.reset();
            api_.reset();
        }
    }

    void on_quit() override {
        // Prevent new route work first, then stop HTTP handlers before joining
        // filesystem job workers. Running SetFileTime calls are allowed to finish.
        shutting_down_.store(true);
        if (api_) api_->begin_shutdown();

        if (startup_thread_.joinable()) {
            startup_thread_.join();
        }

        if (server_) server_->stop();
        if (api_) api_->stop();
        server_.reset();
        api_.reset();
    }

private:
    std::atomic<bool> shutting_down_{false};
    std::unique_ptr<api::ApiController> api_;
    std::unique_ptr<http::HttpServer> server_;
    std::thread startup_thread_;
};

} // namespace craybridge

static initquit_factory_t<craybridge::BridgeRuntime> g_bridge_runtime_factory;
