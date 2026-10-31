#include "application_impl.hpp"
#include <asterion/system/ipc/local_channel.hpp>
#include <asterion/lab.pb.h>

namespace asterion::terminal {

using namespace std::chrono_literals;

// The Python research service gets saved source settings from here, over its
// private socket: the Tushare credential never passes through the UI, and the
// service keeps it in memory only. Everything else the UI asks the service
// directly.
void Application::Impl::register_lab_commands()
{
    // Where the Electron main process (or the development server) sends the
    // UI's lab requests. It resolves this itself rather than trust a path from
    // the page.
    command("lab.endpoint", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        const auto address = co_await PollFuture{services.serving(node::proto::LAB)};
        auto result = snapshot();
        result.values["lab_endpoint"] = {
            {"endpoint", address.endpoint},
            {"service", address.session}
        };
        co_return result;
    });
    // Hands lab the saved settings of one source: the Tushare credential or
    // the recorder's address.
    command("lab.source.configure", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"provider"});
        const auto provider = text(p, "provider");
        lab::proto::Request request;
        auto& source = *request.mutable_configure_source();
        source.set_provider(provider);
        if (provider == "tushare") {
            auto saved = co_await settings<std::optional<DataCredential>>([&] {
                return data_credentials.find("asterion.data.tushare");
            });
            if (!saved || saved->credential.empty())
                throw std::invalid_argument("save the Tushare credential in Settings first");
            source.set_credential(std::move(saved->credential));
            source.set_requests_per_minute(saved->requests_per_minute);
        } else if (provider == "recorder") {
            const auto endpoint = co_await settings<json>([&] {
                return recorder_endpoint.get();
            });
            if (endpoint.is_null())
                throw std::invalid_argument("save the recorder address in Settings first");
            source.set_host(endpoint.at("host").get<std::string>());
            source.set_port(endpoint.at("port").get<unsigned>());
        } else
            throw std::invalid_argument("this data source needs no configuration");
        const auto address = co_await PollFuture{services.serving(node::proto::LAB)};
        request.set_version(1);
        request.set_service_id(address.session);
        request.set_correlation_id("configure." + unique_process_id());
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
