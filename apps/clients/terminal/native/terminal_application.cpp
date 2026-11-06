#include <asterion/version.hpp>
#include "application_impl.hpp"
#include "application_environment.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/environment.hpp>
#include <array>
#include <stdexcept>
#include <ostream>
#include <streambuf>

namespace asterion::terminal {
namespace {

// The single response worker bounds its temporary encoding before admission
// to the retained-reply budget. No complete, unbounded dump is built first.
class ResponseBuffer final : public std::streambuf
{
public:
    std::string bytes;

private:
    static constexpr std::size_t limit = 128 * 1024 * 1024;

    std::streamsize xsputn(const char* text, std::streamsize size) override
    {
        if (static_cast<std::size_t>(size) > limit - bytes.size())
            throw Error(
                ErrorCode::resource_exhausted,
                "Native response exceeds 128 MiB; command outcome may be unknown");
        bytes.append(text, static_cast<std::size_t>(size));
        return size;
    }

    int_type overflow(int_type value) override
    {
        if (traits_type::eq_int_type(value, traits_type::eof()))
            return traits_type::not_eof(value);
        const auto character = traits_type::to_char_type(value);
        xsputn(&character, 1);
        return value;
    }
};

Payload encode_response(const json& envelope, const PayloadBudget& budget)
{
    ResponseBuffer buffer;
    std::ostream stream(&buffer);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << envelope;
    try {
        return budget.retain(std::move(buffer.bytes));
    } catch (const Error& error) {
        throw Error(
            error.code(),
            "Native response capacity reached; command outcome may be unknown");
    }
}

} // namespace

void fields(const json& object, std::initializer_list<std::string_view> names)
{
    if (!object.is_object() || object.size() != names.size())
        throw std::invalid_argument("request fields do not match the current contract");
    for (const auto name : names)
        if (!object.contains(name))
            throw std::invalid_argument("request is missing a required field");
}

std::string text(const json& object, const char* name, bool allow_empty)
{
    const auto& value = object.at(name);
    if (!value.is_string())
        throw std::invalid_argument("text field has the wrong type");
    auto result = value.get<std::string>();
    if ((!allow_empty && result.empty()) || result.find('\0') != std::string::npos)
        throw std::invalid_argument("text field is empty or contains an invalid character");
    return result;
}

unsigned short port_number(const json& p, const char* name)
{
    const auto raw = text(p, name);
    unsigned int port = 0;
    const auto [end, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), port);
    if (ec != std::errc{} || end != raw.data() + raw.size() || !port || port > 65535)
        throw std::invalid_argument("port must be between 1 and 65535");
    return static_cast<unsigned short>(port);
}

void Application::Impl::command(std::string name, Command handler)
{
    if (!commands.emplace(std::move(name), std::move(handler)).second)
        throw std::logic_error("duplicate Terminal command registration");
}

PolledTask<Application::Impl::Response>
Application::Impl::invoke(const std::string& method, const json& params)
{
    const auto found = commands.find(method);
    const auto operation = found == commands.end() ? "terminal.unknown" : method;
    const auto trace = std::string(current_trace_id());
    const SystemClock clock;
    const auto start = clock.monotonic_now();
    auto finish = [&](bool success) noexcept {
        ++(success ? succeeded : failed);
        // Telemetry allocation or sink failure must not change a business result.
        try {
            log::emit(
                success ? (method == "runtime.snapshot" ? log::Level::debug : log::Level::info)
                        : log::Level::warning,
                "{} {}: trace_id={} duration_ns={}",
                operation,
                success ? "completed" : "failed",
                trace,
                clock.monotonic_now() - start);
        } catch (...) {
        }
    };
    try {
        if (found == commands.end())
            throw Error(ErrorCode::invalid_request, "unsupported command");
        // Selection must remain bound to the same market operation while preparation
        // suspends. Read-only queries do not acquire this gate.
        std::optional<ResetFlag> market_admission;
        if (method == "quotes.open" || method == "quotes.close") {
            if (market_busy)
                throw Error(
                    ErrorCode::conflict,
                    "another market operation is in progress; retry after it completes");
            market_busy = true;
            market_admission.emplace(market_busy);
        }
        auto result = co_await found->second(params);
        finish(true);
        co_return result;
    } catch (...) {
        finish(false);
        throw;
    }
}

Application::Impl::Impl()
{
    // Registration precedes all accepted commands.
    command("runtime.snapshot", [this](const json&) -> PolledTask<Response> {
        co_return snapshot();
    });
    register_connection_commands();
    register_data_commands();
    register_lab_commands();
    log::info("terminal started");
    // The one client certificate Market, Lab and Trading hosts register.
    initialization = service_io
                         .submit<void>([this](std::stop_token) {
                             return settings<void>([this] {
                                 trading_connections.prepare_identity();
                             });
                         })
                         .share();
    refresher = service_io.submit<void>(
        [this](std::stop_token stop) {
            return refresh_loop(stop);
        },
        ServiceIo::Lane::observation);
    settings_refresher = service_io.submit<void>(
        [this](std::stop_token stop) {
            return settings_loop(stop);
        },
        ServiceIo::Lane::observation);
}

Application::Impl::~Impl()
{
    lifetime.request_stop();
    initialization.wait();
    refresher.get();
    settings_refresher.get();
    log::info("terminal stopped");
}

Application::Impl::Publication Application::Impl::capture()
{
    Publication next;
    next.settings = settings_view;
    next.lab = lab.view();
    next.metadata = {
        {"protocol", 1},
        {"product", "Asterion Terminal"},
        {"core", "C++20"},
        {"phase", "ready"},
        {"asset", "futures"},
        // The strategy directory: the user's modules and the built-in ones.
        {"research_directory", (asterion_root() / "research").string()},
        // How each exchange assigns closes to today's and yesterday's
        // positions; the order forms follow the core's rule.
        {"close_policies",
            [] {
                json policies = json::object();
                for (const auto* venue : {"SHFE", "INE", "CFFEX", "DCE", "CZCE", "GFEX"})
                    switch (close_policy(venue)) {
                        case ClosePolicy::explicit_buckets:
                            policies[venue] = "explicit_buckets";
                            break;
                        case ClosePolicy::today_first:
                            policies[venue] = "today_first";
                            break;
                        case ClosePolicy::yesterday_first:
                            policies[venue] = "yesterday_first";
                            break;
                    }
                return policies;
            }()},
        {"diagnostics",
            {{"version", build::version},
                {"commit", build::commit},
                {"succeeded", succeeded},
                {"log_directory", asterion_root().string()},
                {"failed", failed},
                {"log_failures", log::failures()},
                {"refresh_failures", refresh_failures},
                {"refresh_failed", refresh_failed}}}
    };
    return next;
}

bool Application::Impl::Publication::same_state(const Publication& other) const
{
    return metadata == other.metadata && lab == other.lab && settings == other.settings;
}

json Application::Impl::Publication::render() const
{
    auto result = metadata;
    result["lab"] = lab;
    result["trading_connections"] = settings->trading;
    result["client_fingerprint"] = settings->client_fingerprint;
    result["quotes_endpoint"] = settings->quotes_endpoint;
    result["history_endpoint"] = settings->history_endpoint;
    result["revision"] = revision;
    result["refreshed_at_ms"] = refreshed_at_ms;
    return result;
}

json Application::Impl::Response::render() &&
{
    if (!publication)
        return std::move(values);
    auto result = publication->render();
    for (auto& [key, value] : values.items())
        result[key] = std::move(value);
    return result;
}

void Application::Impl::publish()
{
    auto next = capture();
    next.revision = cache ? cache->revision + !next.same_state(*cache) : 1;
    next.refreshed_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                               .count();
    cache = std::make_shared<const Publication>(std::move(next));
}

Application::Impl::Response Application::Impl::snapshot()
{
    publish();
    return read_published(json::object());
}

Application::Impl::Response Application::Impl::read_published(const json& params)
{
    if (!cache)
        publish();
    const auto published = cache;
    if (params.contains("since") && params.at("since") == published->revision)
        return json{
            {"unchanged", true},
            {"revision", published->revision},
            {"refreshed_at_ms", published->refreshed_at_ms}
        };
    return Response{published};
}

void Application::Impl::record_refresh_failure(ErrorCode code) noexcept
{
    refresh_failed = true;
    const auto count = ++refresh_failures;
    // Repeated failures are logged at counts 1, 2, 4, 8, ... only.
    if ((count & (count - 1)) == 0)
        log::warn("snapshot refresh failed: {} ({} failures)", error_name(code), count);
    try {
        if (cache) {
            auto next = std::make_shared<Publication>(*cache);
            auto& diagnostics = next->metadata.at("diagnostics");
            diagnostics["refresh_failed"] = true;
            diagnostics["refresh_failures"] = count;
            diagnostics["log_failures"] = log::failures();
            ++next->revision;
            cache = std::move(next);
        }
    } catch (...) {
        // Status allocation failure must not stop the I/O owner.
    }
}

PolledTask<void> Application::Impl::initialized()
{
    co_await PollUntil{[this] {
        return initialization.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }};
    initialization.get();
}

PolledTask<void> Application::Impl::refresh_loop(std::stop_token stop)
{
    try {
        co_await initialized();
    } catch (const std::exception& error) {
        record_refresh_failure(classify(error));
        co_return;
    } catch (...) {
        record_refresh_failure(ErrorCode::internal_error);
        co_return;
    }
    while (!stop.stop_requested() && !lifetime.stop_requested()) {
        try {
            refresh_failed = settings_failed;
            publish();
        } catch (const std::exception& error) {
            record_refresh_failure(classify(error));
        } catch (...) {
            record_refresh_failure(ErrorCode::internal_error);
        }
        const auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        co_await PollUntil{[&] {
            return stop.stop_requested() || lifetime.stop_requested()
                || std::chrono::steady_clock::now() >= next;
        }};
    }
}

PolledTask<void> Application::Impl::settings_loop(std::stop_token stop)
{
    try {
        co_await initialized();
    } catch (const std::exception& error) {
        record_refresh_failure(classify(error));
        co_return;
    } catch (...) {
        record_refresh_failure(ErrorCode::internal_error);
        co_return;
    }
    while (!stop.stop_requested() && !lifetime.stop_requested()) {
        const auto next = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        co_await PollUntil{[&] {
            return stop.stop_requested() || lifetime.stop_requested()
                || std::chrono::steady_clock::now() >= next;
        }};
        if (stop.stop_requested() || lifetime.stop_requested())
            co_return;
        if (settings_busy)
            continue;
        try {
            co_await settings<void>([] {});
            settings_failed = false;
        } catch (const std::exception& error) {
            settings_failed = true;
            record_refresh_failure(classify(error));
        } catch (...) {
            settings_failed = true;
            record_refresh_failure(ErrorCode::internal_error);
        }
    }
}

PolledTask<Application::Impl::Response> Application::Impl::dispatch(const json& request)
{
    fields(request, {"version", "method", "params"});
    if (request.at("version") != 1 || !request.at("version").is_number_integer())
        throw std::invalid_argument("unsupported API version");
    const auto method = text(request, "method");
    const auto& params = request.at("params");
    if (method == "runtime.snapshot") {
        if (!params.is_object() || params.size() > 1
            || (params.size() == 1 && !params.contains("since")))
            throw std::invalid_argument("request fields do not match the current contract");
        if (params.contains("since") && !params.at("since").is_number_unsigned())
            throw std::invalid_argument("snapshot revisions must be unsigned integers");
        if (params.contains("since") && cache)
            co_return read_published(params);
    }
    co_await initialized();
    co_return co_await invoke(method, method == "runtime.snapshot" ? json::object() : params);
}

Application::Application()
{
    // The Terminal's program stays inside the application; its configuration,
    // logs and data are in <root>/terminal. Resolved once, before clients or
    // background work can log.
    const auto root = asterion_root();
    if (!root.is_absolute())
        throw std::invalid_argument("invalid Asterion root directory");
    const auto terminal = root / "terminal";
    if (std::filesystem::is_symlink(root))
        throw std::invalid_argument("invalid Asterion root directory");
    create_directories_durably(terminal / "data");
    // Its logging configuration before its first record; never overwritten.
    install_configuration(root, configuration_templates());
    log::init(terminal, "terminal");
    installation_use_ =
        std::make_unique<FileLock>(terminal / "data", "terminal.lock", FileLock::Access::shared);
    impl_ = std::make_unique<Impl>();
}

Application::~Application()
{
    // Admitted commands still use impl_ and its state after each suspension.
    // Drain them while both are alive, before unique_ptr clears the pointer.
    impl_->lifetime.request_stop();
    impl_->service_io.stop();
}

json Application::dispatch(const json& request)
{
    const auto trace = (request.contains("method") && request.at("method") == "runtime.snapshot")
        ? std::string{}
        : impl_->request_ids.next();
    TraceScope context(trace);
    return impl_->service_io
        .submit<json>(
            [this, request](std::stop_token) -> PolledTask<json> {
                auto response = co_await impl_->dispatch(request);
                co_return co_await impl_->service_io.read<json>(
                    [&] {
                        return std::move(response).render();
                    },
                    ServiceIo::ReadLane::response);
            },
            ServiceIo::Lane::application)
        .get();
}

std::future<void> Application::request(std::string wire, Completion complete)
{
    const auto trace = impl_->request_ids.next();
    TraceScope context(trace);
    return impl_->service_io.submit<void>(
        [this, wire = std::move(wire), complete = std::move(complete)](
            std::stop_token) -> PolledTask<void> {
            Payload encoded;
            std::exception_ptr failure;
            try {
                std::optional<Impl::Response> response;
                std::exception_ptr command_error;
                auto budget = impl_->command_payloads;
                try {
                    if (wire.size() > 65536 || wire.find('\0') != std::string::npos)
                        throw std::invalid_argument("invalid native API request");
                    auto input = parse_json(wire);
                    if (input.is_object() && input.value("method", json{}) == "runtime.snapshot")
                        budget = impl_->snapshot_payloads;
                    response.emplace(co_await impl_->dispatch(input));
                } catch (...) {
                    command_error = std::current_exception();
                }
                co_await impl_->service_io.read<void>(
                    [&] {
                        json envelope = json::object();
                        try {
                            if (command_error)
                                std::rethrow_exception(command_error);
                            envelope["result"] = std::move(*response).render();
                        } catch (const std::exception& error) {
                            envelope["error"] = {
                                {"code", error_name(classify(error))},
                                {"message", error.what()}
                            };
                        } catch (...) {
                            envelope["error"] = {
                                {"code", "internal_error"},
                                {"message", "native core call failed"}
                            };
                        }
                        encoded = encode_response(envelope, budget);
                    },
                    ServiceIo::ReadLane::response);
            } catch (...) {
                failure = std::current_exception();
            }
            complete(std::move(encoded), failure);
        },
        ServiceIo::Lane::application);
}

} // namespace asterion::terminal
