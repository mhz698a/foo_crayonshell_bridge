#include <SDK/foobar2000.h>

#include "api_router.h"
#include "http_server.h"
#include "version.h"

#include <memory>

DECLARE_COMPONENT_VERSION(
    "CrayonShell File Properties Bridge",
    CRAYONSHELL_BRIDGE_VERSION,
    "Loopback-only local file metadata API for CrayonShell."
);

VALIDATE_COMPONENT_FILENAME("foo_crayonshell_bridge.dll");

namespace craybridge {

class BridgeRuntime final : public initquit {
public:
    void on_init() override {
        try {
            api_ = std::make_unique<api::ApiController>();
            server_ = std::make_unique<http::HttpServer>(
                [this](const http::Request& request) {
                    if (!api_) {
                        return http::HttpResponse{
                            503,
                            "{\"success\":false,\"error\":{"
                            "\"code\":\"SERVICE_STOPPING\","
                            "\"message\":\"The local metadata service is stopping.\"}}"
                        };
                    }
                    return api_->handle(request);
                });

            const auto started = server_->start();
            if (!started.success) {
                server_.reset();
            }

            // The port/token descriptor is not exposed until the trusted
            // foo_ui_webview2 bootstrap is implemented and verified.
        } catch (...) {
            if (api_) api_->begin_shutdown();
            if (server_) server_->stop();
            server_.reset();
            api_.reset();
        }
    }

    void on_quit() override {
        if (api_) api_->begin_shutdown();
        if (server_) server_->stop();
        server_.reset();
        api_.reset();
    }

private:
    std::unique_ptr<api::ApiController> api_;
    std::unique_ptr<http::HttpServer> server_;
};

} // namespace craybridge

static initquit_factory_t<craybridge::BridgeRuntime> g_bridge_runtime_factory;
