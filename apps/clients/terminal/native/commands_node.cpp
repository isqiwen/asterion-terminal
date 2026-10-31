#include "application_impl.hpp"

namespace asterion::terminal {

void Application::Impl::register_node_commands()
{
    // A local service's run state, kept across Terminal restarts.
    command("node.action", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"service", "action"});
        co_await PollFuture{services.act(text(p, "service"), text(p, "action"))};
        co_return snapshot();
    });
}

} // namespace asterion::terminal
