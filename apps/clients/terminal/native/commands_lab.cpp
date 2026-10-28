#include "application_impl.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/lab.pb.h>

namespace asterion::terminal {

using namespace std::chrono_literals;

// The Python research service gets the saved Tushare credential from here, over
// its private socket: the credential never passes through the UI, and the
// service keeps it in memory only. Everything else the UI asks the service
// directly.
void Application::Impl::register_lab_commands()
{
    command("lab.source.configure", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        auto saved = co_await settings<std::optional<DataCredential>>([&] {
            return data_credentials.find("asterion.data.tushare");
        });
        if (!saved || saved->credential.empty())
            throw std::invalid_argument("save the Tushare credential in Settings first");
        const auto node = co_await local_node_client();
        const auto address = co_await PollFuture{node->service_endpoint("lab", node::proto::LAB)};
        lab::proto::Request request;
        request.set_version(1);
        request.set_service_id(address.session);
        request.set_correlation_id("configure." + unique_process_id());
        auto& source = *request.mutable_configure_source();
        source.set_provider("tushare");
        source.set_credential(std::move(saved->credential));
        source.set_requests_per_minute(saved->requests_per_minute);
        const auto reply = co_await service_io.admin<lab::proto::Response>(
            [endpoint = address.endpoint, payload = request.SerializeAsString()] {
                auto channel = ipc::Channel::connect(endpoint, 2s);
                channel.send(payload, 2s);
                lab::proto::Response response;
                if (!response.ParseFromString(channel.receive(10s)))
                    throw Error(ErrorCode::operation_failed, "invalid lab service reply");
                return response;
            });
        if (reply.correlation_id() != request.correlation_id())
            throw Error(ErrorCode::operation_failed, "lab service reply identity mismatch");
        if (reply.has_error())
            throw_remote_error(reply.error().code(), reply.error().message());
        co_return snapshot();
    });
}

} // namespace asterion::terminal
