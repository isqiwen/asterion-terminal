#include "local_services.hpp"
#include "application_environment.hpp"
#include "local_service_health.hpp"
#include "node_paths.hpp"
#include <asterion/base/error.hpp>
#include <asterion/base/id.hpp>
#include <asterion/base/serialization.hpp>
#include <asterion/system/durable_file.hpp>
#include <asterion/system/environment.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/file_lock.hpp>
#include <asterion/system/process/owner.hpp>
#include <asterion/protocol/health.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <mutex>
#include <poll.h>
#include <set>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace asterion::terminal {
namespace fs = std::filesystem;
namespace wire = node::proto;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

std::string utf8(const fs::path& path)
{
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

struct Kind
{
    wire::ServiceKind kind;
    const char *service, *program, *executable, *name;
};

// One service per kind, in the order the destructor stops them.
constexpr std::array<Kind, 1> kinds{{
    {wire::LAB, "lab", "lab", "", "lab"},
}};

const Kind& kind_of(wire::ServiceKind kind)
{
    for (const auto& item : kinds)
        if (item.kind == kind)
            return item;
    throw std::invalid_argument("the Terminal runs no such local service");
}

const Kind& service_named(const std::string& service)
{
    for (const auto& item : kinds)
        if (item.service == service)
            return item;
    throw Error(ErrorCode::not_found, "no such local service: " + service);
}

struct Record
{
    wire::ServiceKind kind;
    bool desired = false;

    Json json() const
    {
        return {{"kind", kind_of(kind).name}, {"desired", desired}};
    }
};

struct Service
{
    explicit Service(Record value)
        : record(std::move(value))
    {}

    Record record;
    std::unique_ptr<ChildProcess> process;
    protocol::ServiceHealth health = protocol::ServiceHealth::starting;
    std::optional<runtime::proto::ExecutionHealth> execution;
    Clock::time_point execution_observed{}, started{}, probe{}, retry{};
    std::int64_t last_heartbeat = 0;
    unsigned failures = 0, restarts = 0;
    std::string error;
    // Callers of start() waiting for the process to answer its first heartbeat.
    std::vector<std::promise<ServiceEndpoint>> waiting;
};

} // namespace

struct LocalServices::Impl
{
    std::function<std::optional<fs::path>()> python;
    const fs::path root = asterion_root();
    const fs::path state = program_directory("terminal") / "data";
    std::unique_ptr<FileLock> owner;
    const std::string instance = unique_process_id();
    fs::path sockets;
    std::string node_id;
    const Clock::time_point began = Clock::now();

    // Owned by the supervisor thread.
    std::map<std::string, Service> services;

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> work;
    std::shared_ptr<const wire::Status> published;
    bool stopping = false;
    std::jthread supervisor;

    fs::path record_file(const std::string& service) const
    {
        return state / "services" / (service + ".json");
    }

    std::string endpoint_of(const std::string& service) const
    {
        return utf8(sockets / (service + ".sock"));
    }

    void save(const std::string& service, const Record& record) const
    {
        create_directories_durably(record_file(service).parent_path());
        replace_file_durably(record_file(service), record.json().dump());
    }

    // Reads the run intent of a service, a file named by the service.
    Record load(const Kind& kind, const fs::path& file) const
    {
        std::ifstream input(file, std::ios::binary);
        const std::string raw{std::istreambuf_iterator<char>(input), {}};
        try {
            const auto value = parse_json(raw, 65536);
            if (value.at("kind").get<std::string>() != kind.name)
                throw std::invalid_argument("service name and kind differ");
            return Record{kind.kind, value.at("desired").get<bool>()};
        } catch (const std::exception& error) {
            throw std::invalid_argument(
                "invalid local service record " + file.string() + ": " + error.what());
        }
    }

    // The program's directories and its logging, seeded once from the
    // application's defaults.
    void prepare_program(const Kind& kind) const
    {
        if (const auto builtin = environment_path("ASTERION_BUILTIN_RESEARCH"))
            sync_builtin_research(*builtin, asterion_root() / "research");
        const auto folder = program_directory(kind.program);
        for (const auto* part : {"bin", "lib", "config", "logs", "data"})
            create_directories_durably(folder / part);
        const auto seed = [&](const fs::path& target, const char* name) {
            if (fs::exists(target))
                return;
            const auto source = node_configuration_templates() / name;
            std::ifstream input(source, std::ios::binary);
            if (!input)
                throw std::runtime_error("cannot read configuration default: " + source.string());
            create_directories_durably(target.parent_path());
            replace_file_durably(target, std::string{std::istreambuf_iterator<char>(input), {}});
        };
        seed(folder / "config" / "logging.json", "logging.json");
    }

    std::unique_ptr<ChildProcess> launch(const Kind& kind)
    {
        prepare_program(kind);
        fs::path executable;
        std::vector<std::string> args;
        const auto interpreter = python();
        if (!interpreter || !fs::is_regular_file(*interpreter))
            throw Error(
                ErrorCode::unavailable,
                "lab needs the Python runtime; it is installed at the first launch");
        executable = *interpreter;
        // Isolated from the user's environment and site; no bytecode written.
        args = {"-I", "-B", "-m", "asterion_lab"};
        const auto endpoint = endpoint_of(kind.service);
        const auto health = utf8(sockets / (std::string(kind.service) + ".health"));
        fs::remove(endpoint);
        fs::remove(health);
        const auto directory = program_directory(kind.program) / "data" / kind.service;
        create_directories_durably(directory);
        args.insert(
            args.end(),
            {"--owner-pid",
                std::to_string(current_process_id()),
                "--session",
                kind.service,
                "--directory",
                utf8(directory),
                "--terminal-data",
                utf8(program_directory("terminal") / "data"),
                "--health-endpoint",
                health,
                "--endpoint",
                endpoint});
        return std::make_unique<ChildProcess>(executable, args);
    }

    void start(const std::string& name, Service& service, bool automatic)
    {
        const auto& kind = service_named(name);
        if (automatic)
            ++service.restarts;
        service.process.reset();
        try {
            service.process = launch(kind);
            log::info("service {} started: pid={}", name, service.process->id());
            service.health = protocol::ServiceHealth::starting;
            service.last_heartbeat = 0;
            service.execution.reset();
            service.failures = 0;
            service.started = Clock::now();
            service.probe = service.started + 200ms;
            service.error.clear();
        } catch (const std::exception& error) {
            service.process.reset();
            service.error = error.what();
            log::error("service {} did not start: {}", name, error.what());
        }
        service.retry = Clock::now() + 5s;
    }

    void stop(Service& service)
    {
        if (service.process) {
            service.process->request_stop();
            service.process.reset();
        }
    }

    void supervise(const std::string& name, Service& service)
    {
        if (service.process && service.process->exited()) {
            log::warn("service {} exited: exit_code={}", name, service.process->exit_code());
            service.process.reset();
            service.error = "process exited; awaiting bounded restart";
        }
        if (service.process && Clock::now() >= service.probe) {
            service.probe = Clock::now() + 5s;
            std::string error;
            HealthObservation observation;
            try {
                auto probe = probe_service_health(
                    service.record.kind,
                    name,
                    utf8(sockets / (name + ".health")));
                while (!probe.poll())
                    std::this_thread::sleep_for(1ms);
                observation = probe.take();
            } catch (const std::exception& failure) {
                error = failure.what();
            }
            if (error.empty()) {
                service.health = observation.status;
                service.execution = std::move(observation.execution);
                service.execution_observed = Clock::now();
                if (service.health == protocol::ServiceHealth::starting)
                    service.probe = Clock::now() + 250ms;
                service.last_heartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                                             .count();
                service.failures = 0;
                service.error.clear();
            } else if (!service.last_heartbeat && Clock::now() - service.started < 10s) {
                service.probe = Clock::now() + 250ms;
                service.error = "waiting for the first heartbeat: " + error;
            } else {
                service.health = protocol::ServiceHealth::unresponsive;
                service.error = "service heartbeat unavailable";
                log::warn(
                    "service {} heartbeat failed ({} in a row): {}",
                    name,
                    service.failures + 1,
                    error);
                if (++service.failures >= 3) {
                    stop(service);
                    service.retry = Clock::now() + 5s;
                }
            }
        }
        if (service.record.desired && !service.process && service.restarts < 3
            && Clock::now() >= service.retry)
            // The first start of a service that should run is not a restart.
            start(name, service, service.retry != Clock::time_point{});
    }

    // A started service is usable once it answers a heartbeat; until then its
    // socket may not exist yet. Waiting callers fail when it stops running or
    // never answers.
    void answer_waiting(const std::string& name, Service& service)
    {
        if (service.waiting.empty())
            return;
        const bool ready = service.process && service.last_heartbeat
            && service.health != protocol::ServiceHealth::unresponsive;
        const bool failed =
            !service.process || service.health == protocol::ServiceHealth::unresponsive;
        if (!ready && !failed)
            return;
        for (auto& promise : service.waiting) {
            if (ready)
                promise.set_value(endpoint(name, service.record.kind));
            else
                promise.set_exception(
                    std::make_exception_ptr(Error(
                        ErrorCode::unavailable,
                        service.error.empty() ? "service " + name + " did not start"
                                              : service.error)));
        }
        service.waiting.clear();
    }

    std::shared_ptr<const wire::Status> report() const
    {
        auto status = std::make_shared<wire::Status>();
        const auto platform = current_platform();
        status->set_instance_id(instance);
        status->set_node_id(node_id);
        status->set_phase(wire::Status::READY);
        status->mutable_execution()->set_business_ready(true);
        status->set_pid(current_process_id());
        status->set_os(platform.os);
        status->set_arch(platform.arch);
        status->set_version(ASTERION_PRODUCT_VERSION);
        status->set_uptime_ms(
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began)
                    .count()));
        for (const auto& [name, service] : services) {
            auto* item = status->add_services();
            item->set_id(name);
            item->set_kind(service.record.kind);
            item->set_desired_running(service.record.desired);
            item->set_restarts(service.restarts);
            item->set_error(service.error);
            const bool running = service.process != nullptr;
            item->set_state(
                running                       ? "running"
                    : !service.record.desired ? "stopped"
                    : service.restarts >= 3   ? "failed"
                                              : "restarting");
            item->set_pid(running ? service.process->id() : 0);
            item->set_health(
                std::string(
                    protocol::service_health_name(
                        running ? service.health : protocol::ServiceHealth::offline)));
            item->set_last_heartbeat_ms(service.last_heartbeat);
            if (running && service.execution) {
                *item->mutable_execution() = *service.execution;
                protocol::age_execution_health(
                    *item->mutable_execution(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        Clock::now() - service.execution_observed)
                        .count());
                if (service.health != protocol::ServiceHealth::unresponsive
                    && protocol::execution_health_stalled(item->execution()))
                    item->set_health(
                        std::string(
                            protocol::service_health_name(protocol::ServiceHealth::degraded)));
            }
            item->set_endpoint(endpoint_of(name));
            item->set_directory(
                utf8(program_directory(service_named(name).program) / "data" / name));
        }
        return status;
    }

    void publish()
    {
        auto next = report();
        std::lock_guard lock(mutex);
        published = std::move(next);
    }

    // Runs `task` on the supervisor thread and returns its result.
    template <class T>
    std::future<T> post(std::function<T()> task)
    {
        auto job = std::make_shared<std::packaged_task<T()>>(std::move(task));
        auto result = job->get_future();
        {
            std::lock_guard lock(mutex);
            if (stopping)
                throw Error(ErrorCode::unavailable, "local services are stopping");
            work.push_back([job] {
                (*job)();
            });
        }
        wake.notify_one();
        return result;
    }

    void run(std::stop_token)
    {
        for (;;) {
            std::deque<std::function<void()>> jobs;
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, 200ms, [&] {
                    return stopping || !work.empty();
                });
                if (stopping)
                    break;
                jobs.swap(work);
            }
            for (auto& job : jobs)
                job();
            for (auto& [name, service] : services) {
                supervise(name, service);
                answer_waiting(name, service);
            }
            publish();
        }
        for (const auto& kind : kinds)
            if (const auto found = services.find(kind.service); found != services.end()) {
                stop(found->second);
                found->second.error = "local services are stopping";
                answer_waiting(kind.service, found->second);
            }
        publish();
    }

    explicit Impl(std::function<std::optional<fs::path>()> interpreter)
        : python(std::move(interpreter))
    {
        create_directories_durably(state);
        owner = std::make_unique<FileLock>(state, "services.lock");
        const auto identity = state / "node-id";
        if (fs::exists(identity)) {
            std::ifstream input(identity);
            std::getline(input, node_id);
            validate_id(node_id);
        } else {
            node_id = "local-" + unique_process_id().substr(0, 16);
            replace_file_durably(identity, node_id);
        }
        for (const auto& kind : kinds)
            if (const auto file = record_file(kind.service); fs::exists(file))
                services.emplace(kind.service, Service{load(kind, file)});
        // Short and private: socket paths have a small length limit.
        sockets = fs::path("/tmp") / ("ast-" + instance.substr(0, 12));
        if (!fs::create_directory(sockets))
            throw std::runtime_error("cannot create the private service socket directory");
        fs::permissions(sockets, fs::perms::owner_all, fs::perm_options::replace);
        publish();
        supervisor = std::jthread([this](std::stop_token stop) {
            run(stop);
        });
    }

    ~Impl()
    {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        if (supervisor.joinable())
            supervisor.join();
        std::error_code ignored;
        fs::remove_all(sockets, ignored);
    }

    std::future<ServiceEndpoint> launch(wire::ServiceKind kind, bool answering);

    ServiceEndpoint endpoint(const std::string& name, wire::ServiceKind kind) const
    {
        const auto found = services.find(name);
        if (found == services.end())
            throw Error(ErrorCode::not_found, "no such local service: " + name);
        if (found->second.record.kind != kind)
            throw std::invalid_argument("service kind mismatch");
        if (!found->second.process)
            throw Error(
                ErrorCode::unavailable,
                found->second.error.empty() ? "service process is not running"
                                            : found->second.error);
        return ServiceEndpoint{"", name, 0, endpoint_of(name), "local", node_id};
    }
};

LocalServices::LocalServices(std::function<std::optional<fs::path>()> python)
    : impl_(std::make_unique<Impl>(std::move(python)))
{}

LocalServices::~LocalServices() = default;

NodeSnapshot LocalServices::view() const
{
    std::lock_guard lock(impl_->mutex);
    return NodeSnapshot{
        .id = "local",
        .host = "localhost",
        .online = true,
        .last_heartbeat_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count(),
        .health = impl_->published
    };
}

std::future<ServiceEndpoint> LocalServices::start(wire::ServiceKind kind)
{
    return impl_->launch(kind, false);
}

std::future<ServiceEndpoint> LocalServices::serving(wire::ServiceKind kind)
{
    return impl_->launch(kind, true);
}

std::future<ServiceEndpoint> LocalServices::Impl::launch(wire::ServiceKind kind, bool answering)
{
    auto promise = std::make_shared<std::promise<ServiceEndpoint>>();
    auto result = promise->get_future();
    (void)post<void>([this, kind, promise, answering] {
        auto& impl = *this;
        try {
            const auto& item = kind_of(kind);
            auto found = impl.services.find(item.service);
            if (found == impl.services.end()) {
                Record record{kind, true};
                impl.save(item.service, record);
                found = impl.services.emplace(item.service, Service{std::move(record)}).first;
            }
            auto& service = found->second;
            if (!service.record.desired) {
                auto next = service.record;
                next.desired = true;
                impl.save(item.service, next);
                service.record = std::move(next);
            }
            if (!service.process) {
                service.restarts = 0;
                impl.start(item.service, service, false);
            }
            if (!answering) {
                promise->set_value(impl.endpoint(item.service, kind));
                impl.publish();
                return;
            }
            // From here the service answers the caller.
            service.waiting.push_back(std::move(*promise));
        } catch (...) {
            promise->set_exception(std::current_exception());
            return;
        }
        impl.answer_waiting(kind_of(kind).service, impl.services.at(kind_of(kind).service));
        impl.publish();
    });
    return result;
}

std::future<ServiceEndpoint> LocalServices::endpoint(std::string service, wire::ServiceKind kind)
{
    return impl_->post<ServiceEndpoint>([this, service = std::move(service), kind] {
        return impl_->endpoint(service, kind);
    });
}

std::future<void> LocalServices::act(std::string name, std::string action)
{
    return impl_->post<void>([this, name = std::move(name), action = std::move(action)] {
        auto& impl = *impl_;
        const auto found = impl.services.find(name);
        if (found == impl.services.end())
            throw Error(ErrorCode::not_found, "no such local service: " + name);
        if (action != "start" && action != "stop" && action != "restart")
            throw std::invalid_argument("unknown service action");
        auto& service = found->second;
        auto next = service.record;
        next.desired = action != "stop";
        impl.save(name, next);
        service.record = std::move(next);
        if (action == "stop") {
            impl.stop(service);
            service.error.clear();
        } else if (action == "restart" || !service.process) {
            service.restarts = 0;
            impl.stop(service);
            impl.start(name, service, false);
        }
        impl.publish();
    });
}

} // namespace asterion::terminal
