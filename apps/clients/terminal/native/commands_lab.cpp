#include "application_impl.hpp"

namespace asterion::terminal {

void Application::Impl::register_lab_commands()
{
    // The first launch installs the Python runtime; the page polls its progress.
    // A development or test entry names its prepared interpreter: that runtime is ready.
    const auto runtime = [this] {
        return environment_path("ASTERION_PYTHON")
            ? json{{"state", "ready"}, {"error", ""}, {"modules", json::array()}}
            : python_runtime.status();
    };
    command("runtime.python.status", [this, runtime](const json& p) -> PolledTask<Response> {
        fields(p, {});
        auto result = snapshot();
        result.values["python_runtime"] = runtime();
        co_return result;
    });
    command("runtime.python.install", [this, runtime](const json& p) -> PolledTask<Response> {
        fields(p, {});
        if (!environment_path("ASTERION_PYTHON"))
            python_runtime.install();
        auto result = snapshot();
        result.values["python_runtime"] = runtime();
        co_return result;
    });
    // Where the Electron main process (or the development server) sends the
    // UI's lab requests. It resolves this itself rather than trust a path from
    // the page.
    command("lab.endpoint", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        const auto address = co_await PollFuture{lab.serving()};
        auto result = snapshot();
        result.values["lab_endpoint"] = {
            {"endpoint", address.endpoint},
            {"service", address.session}
        };
        co_return result;
    });
}

} // namespace asterion::terminal
