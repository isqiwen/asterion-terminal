#include "local_services.hpp"
#include "application_environment.hpp"
#include "local_service_health.hpp"
#include "node_paths.hpp"
#include "plugin_catalog.hpp"
#include "service_programs.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/foundation/serialization.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/log.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/protocol/health.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <deque>
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

// One service per kind, and the order the destructor stops them in: the
// services that read Data before Data itself.
constexpr std::array<Kind, 4> kinds{{
    {wire::LAB, "lab", "lab", "", "lab"},
    {wire::RESEARCH, "research", "research", "asterion_research", "research"},
    {wire::MARKET, "market-data", "market", "asterion_market", "market"},
    {wire::DATA, "historical-data", "data", "asterion_data", "data"},
}};

const Kind& kind_of(wire::ServiceKind kind)
{
    for (const auto& item : kinds)
        if (item.kind == kind)
            return item;
    throw std::invalid_argument("the Terminal runs no such local service");
}

const Kind& kind_named(const std::string& name)
{
    for (const auto& item : kinds)
        if (item.name == name)
            return item;
    throw std::invalid_argument("invalid local service record");
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
    std::vector<std::string> plugins;

    Json json() const
    {
        return {{"kind", kind_of(kind).name}, {"desired", desired}, {"plugins", plugins}};
    }

    // What a plugin change must have seen: the selection and whether it runs.
    std::string revision() const
    {
        return sha256_bytes(json().dump());
    }
};

struct Service
{
    Record record;
    std::unique_ptr<ChildProcess> process;
    protocol::ServiceHealth health = protocol::ServiceHealth::starting;
    std::optional<runtime::proto::ExecutionHealth> execution;
    Clock::time_point execution_observed{}, started{}, probe{}, retry{};
    std::int64_t last_heartbeat = 0;
    unsigned failures = 0, restarts = 0;
    std::string error, artifact;
};

void write_frame(int fd, const std::string& payload)
{
    const auto size = static_cast<std::uint32_t>(payload.size());
    const std::array<unsigned char, 4> header{
        static_cast<unsigned char>(size >> 24),
        static_cast<unsigned char>(size >> 16),
        static_cast<unsigned char>(size >> 8),
        static_cast<unsigned char>(size)
    };
    std::string bytes(reinterpret_cast<const char*>(header.data()), header.size());
    bytes += payload;
    for (std::size_t sent = 0; sent < bytes.size();) {
        const auto written = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (written <= 0) {
            if (written < 0 && errno == EINTR)
                continue;
            throw std::runtime_error("discovery reply failed");
        }
        sent += static_cast<std::size_t>(written);
    }
}

// Reads exactly `size` bytes within the deadline.
void read_exactly(int fd, char* data, std::size_t size, Clock::time_point deadline)
{
    for (std::size_t got = 0; got < size;) {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
        pollfd descriptor{fd, POLLIN, 0};
        if (left.count() <= 0 || ::poll(&descriptor, 1, static_cast<int>(left.count())) <= 0)
            throw std::runtime_error("discovery request timed out");
        const auto read = ::recv(fd, data + got, size - got, 0);
        if (read <= 0) {
            if (read < 0 && errno == EINTR)
                continue;
            throw std::runtime_error("discovery peer closed");
        }
        got += static_cast<std::size_t>(read);
    }
}

} // namespace

struct LocalServices::Impl
{
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
    std::jthread supervisor, discovery;
    int listener = -1;

    fs::path record_file(const std::string& service) const
    {
        return state / "services" / (service + ".json");
    }

    std::string endpoint_of(const std::string& service) const
    {
        return utf8(sockets / (service + ".sock"));
    }

    std::string discovery_endpoint() const
    {
        return utf8(sockets / "discovery.sock");
    }

    void save(const std::string& service, const Record& record) const
    {
        create_directories_durably(record_file(service).parent_path());
        replace_file_durably(record_file(service), record.json().dump());
    }

    Record load(const fs::path& file) const
    {
        std::ifstream input(file, std::ios::binary);
        const std::string raw{std::istreambuf_iterator<char>(input), {}};
        try {
            const auto value = parse_json(raw, 65536);
            require_fields(value, {"kind", "desired", "plugins"});
            Record record{kind_named(value.at("kind").get<std::string>()).kind};
            record.desired = value.at("desired").get<bool>();
            record.plugins = value.at("plugins").get<std::vector<std::string>>();
            for (const auto& hash : record.plugins)
                if (hash.size() != 64
                    || hash.find_first_not_of("0123456789abcdef") != std::string::npos)
                    throw std::invalid_argument("invalid plugin digest");
            if (kind_of(record.kind).service != file.stem().string())
                throw std::invalid_argument("service name and kind differ");
            return record;
        } catch (const std::exception& error) {
            throw std::invalid_argument(
                "invalid local service record " + file.string() + ": " + error.what());
        }
    }

    // A plugin is kept by its digest for as long as a selection may name it.
    fs::path plugin_file(const std::string& hash) const
    {
        return state / "plugins" / (hash + ".bin");
    }

    void keep_plugin(const PluginArtifact& artifact) const
    {
        const auto file = plugin_file(artifact.sha256);
        if (fs::exists(file) && sha256_file(file) == artifact.sha256)
            return;
        create_directories_durably(file.parent_path());
        std::ifstream input(artifact.path, std::ios::binary);
        replace_file_durably(file, std::string{std::istreambuf_iterator<char>(input), {}});
        if (sha256_file(file) != artifact.sha256)
            throw std::invalid_argument(
                "native plugin changed while it was kept: " + file.string());
    }

    // Exactly the selected plugins, copied by digest into the service's lib/.
    fs::path materialize(const Kind& kind, const std::vector<std::string>& plugins) const
    {
        const auto folder = program_directory(kind.program) / "lib" / "plugins" / kind.service;
        create_directories_durably(folder);
        std::set<fs::path> expected;
        const auto extension = current_platform().os == "macos" ? ".dylib" : ".so";
        for (const auto& hash : plugins) {
            const auto source = plugin_file(hash);
            if (!fs::exists(source) || sha256_file(source) != hash)
                throw std::invalid_argument(
                    "native plugin " + source.string()
                    + " is missing or corrupted; configure the service's plugins again");
            const auto target = folder / (hash + extension);
            expected.insert(target);
            if (!fs::exists(target) || sha256_file(target) != hash) {
                std::ifstream input(source, std::ios::binary);
                replace_file_durably(
                    target,
                    std::string{std::istreambuf_iterator<char>(input), {}});
            }
        }
        for (const auto& entry : fs::directory_iterator(folder))
            if (!expected.contains(entry.path()))
                fs::remove_all(entry.path());
        return folder;
    }

    // The program's directories, its logging and Research's settings seeded
    // once from the application's defaults.
    void prepare_program(const Kind& kind) const
    {
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
        if (kind.kind == wire::RESEARCH)
            seed(folder / "config" / kind.service / "research.json", "research.json");
    }

    std::unique_ptr<ChildProcess> launch(const Kind& kind, Service& service)
    {
        prepare_program(kind);
        fs::path executable;
        std::vector<std::string> args;
        if (kind.kind == wire::LAB) {
            // Electron and the development entry name the bundled interpreter;
            // an installed application has it beside its native programs.
            const auto python =
                environment_path("ASTERION_PYTHON")
                    .value_or(application_directory().parent_path() / "python" / "bin" / "python3");
            if (!fs::is_regular_file(python))
                throw Error(
                    ErrorCode::unavailable,
                    "lab needs the bundled Python: " + utf8(python));
            executable = python;
            service.artifact = sha256_file(executable);
            // Isolated from the user's environment and site; no bytecode written.
            args = {"-I", "-B", "-m", "asterion_lab"};
        } else {
            const auto programs = local_service_programs(kind.kind);
            executable = program_directory(kind.program) / "bin" / kind.executable;
            link_node_program(programs.executable, executable);
            service.artifact = sha256_file(programs.executable);
        }
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
                "--health-endpoint",
                health,
                "--endpoint",
                endpoint,
                "--node-id",
                node_id});
        if (kind.kind == wire::DATA || kind.kind == wire::RESEARCH)
            args.insert(
                args.end(),
                {"--plugin-directory",
                    utf8(materialize(kind, service.record.plugins)),
                    "--discovery-endpoint",
                    discovery_endpoint()});
        if (kind.kind == wire::DATA)
            args.insert(
                args.end(),
                {"--internal-endpoint", utf8(sockets / (std::string(kind.service) + ".internal"))});
        if (kind.kind == wire::RESEARCH)
            args.insert(
                args.end(),
                {"--config",
                    utf8(
                        program_directory(kind.program) / "config" / kind.service
                        / "research.json")});
        if (kind.kind == wire::MARKET) {
            const auto programs = local_service_programs(kind.kind);
            if (fs::exists(programs.provider))
                args.insert(args.end(), {"--ctp-library", utf8(programs.provider)});
            if (fs::exists(programs.catalog))
                args.insert(args.end(), {"--ctp-catalog-library", utf8(programs.catalog)});
        }
        return std::make_unique<ChildProcess>(executable, args);
    }

    void start(const std::string& name, Service& service, bool automatic)
    {
        const auto& kind = service_named(name);
        if (automatic)
            ++service.restarts;
        service.process.reset();
        try {
            service.process = launch(kind, service);
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

    std::shared_ptr<const wire::Status> report() const
    {
        auto status = std::make_shared<wire::Status>();
        const auto platform = current_platform();
        status->set_instance_id(instance);
        status->set_node_id(node_id);
        status->set_discovery_endpoint(discovery_endpoint());
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
            item->set_artifact(service.artifact);
            item->set_revision(service.record.revision());
            for (const auto& hash : service.record.plugins)
                item->add_plugin_artifacts(hash);
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
            for (auto& [name, service] : services)
                supervise(name, service);
            publish();
        }
        // Readers of Data first, then Data itself.
        for (const auto& kind : kinds)
            if (const auto found = services.find(kind.service); found != services.end())
                stop(found->second);
        publish();
    }

    void answer(int peer) const
    {
        std::array<char, 4> header{};
        const auto deadline = Clock::now() + 2s;
        read_exactly(peer, header.data(), header.size(), deadline);
        const auto size = (std::uint32_t(static_cast<unsigned char>(header[0])) << 24)
            | (std::uint32_t(static_cast<unsigned char>(header[1])) << 16)
            | (std::uint32_t(static_cast<unsigned char>(header[2])) << 8)
            | std::uint32_t(static_cast<unsigned char>(header[3]));
        if (!size || size > 65536)
            throw std::runtime_error("invalid discovery request size");
        std::string payload(size, '\0');
        read_exactly(peer, payload.data(), size, deadline);
        wire::Request request;
        wire::Response response;
        response.set_version(1);
        try {
            if (!request.ParseFromString(payload))
                throw std::invalid_argument("malformed discovery request");
            response.set_correlation_id(request.correlation_id());
            validate_id(request.correlation_id());
            if (request.version() != 1 || !request.has_status())
                throw Error(ErrorCode::permission_denied, "service discovery is read only");
            std::lock_guard lock(mutex);
            if (!published)
                throw Error(ErrorCode::unavailable, "local services are starting");
            *response.mutable_status() = *published;
        } catch (const std::exception& error) {
            response.clear_status();
            response.mutable_error()->set_code(std::string(error_name(classify(error))));
            response.mutable_error()->set_message(error.what());
        }
        write_frame(peer, response.SerializeAsString());
    }

    void serve(std::stop_token stop)
    {
        while (!stop.stop_requested()) {
            pollfd descriptor{listener, POLLIN, 0};
            if (::poll(&descriptor, 1, 200) <= 0)
                continue;
            const int peer = ::accept(listener, nullptr, nullptr);
            if (peer < 0)
                continue;
            try {
                answer(peer);
            } catch (const std::exception& error) {
                log::warn("service discovery request failed: {}", error.what());
            }
            ::close(peer);
        }
    }

    void listen()
    {
        const auto path = discovery_endpoint();
        sockaddr_un address{};
        if (path.size() >= sizeof(address.sun_path))
            throw std::runtime_error("discovery socket path is too long");
        listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listener < 0)
            throw std::runtime_error("cannot create the discovery socket");
        address.sun_family = AF_UNIX;
        std::copy(path.begin(), path.end(), address.sun_path);
        if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0
            || ::listen(listener, 8) != 0)
            throw std::runtime_error("cannot listen on the discovery socket " + path);
    }

    Impl()
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
        if (fs::exists(state / "services"))
            for (const auto& entry : fs::directory_iterator(state / "services"))
                if (entry.path().extension() == ".json")
                    services.emplace(entry.path().stem().string(), Service{load(entry.path())});
        // Short and private: socket paths have a small length limit.
        sockets = fs::path("/tmp") / ("ast-" + instance.substr(0, 12));
        if (!fs::create_directory(sockets))
            throw std::runtime_error("cannot create the private service socket directory");
        fs::permissions(sockets, fs::perms::owner_all, fs::perm_options::replace);
        listen();
        publish();
        discovery = std::jthread([this](std::stop_token stop) {
            serve(stop);
        });
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
        discovery.request_stop();
        if (discovery.joinable())
            discovery.join();
        if (listener >= 0)
            ::close(listener);
        std::error_code ignored;
        fs::remove_all(sockets, ignored);
    }

    std::vector<std::string> bundled_plugins() const
    {
        std::vector<std::string> hashes;
        for (const auto& entry : local_plugin_catalog().entries)
            if (!entry.managed && entry.availability == PluginAvailability::available
                && entry.selectable_for_data_tasks()) {
                keep_plugin(entry.artifact);
                hashes.push_back(entry.artifact.sha256);
            }
        std::sort(hashes.begin(), hashes.end());
        return hashes;
    }

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

LocalServices::LocalServices()
    : impl_(std::make_unique<Impl>())
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

std::future<ServiceEndpoint> LocalServices::start(
    wire::ServiceKind kind,
    std::optional<std::vector<std::string>> research_plugins)
{
    return impl_->post<ServiceEndpoint>([this, kind, research_plugins] {
        auto& impl = *impl_;
        const auto& item = kind_of(kind);
        auto found = impl.services.find(item.service);
        if (found != impl.services.end() && research_plugins)
            throw std::invalid_argument(
                "the local research service already exists; configure its plugins");
        if (found == impl.services.end()) {
            Record record{kind, true};
            if (kind == wire::DATA || kind == wire::RESEARCH) {
                if (research_plugins) {
                    const auto selection =
                        local_plugin_catalog().select_data_task_plugins(*research_plugins);
                    for (const auto& artifact : selection.uploads)
                        impl.keep_plugin(artifact);
                    record.plugins = selection.hashes;
                    std::sort(record.plugins.begin(), record.plugins.end());
                } else
                    record.plugins = impl.bundled_plugins();
            }
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
        impl.publish();
        return impl.endpoint(item.service, kind);
    });
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

std::future<void> LocalServices::configure_plugins(
    std::string name,
    std::string expected,
    std::vector<std::string> plugins)
{
    return impl_->post<void>([this,
                                 name = std::move(name),
                                 expected = std::move(expected),
                                 plugins = std::move(plugins)] {
        auto& impl = *impl_;
        const auto found = impl.services.find(name);
        if (found == impl.services.end())
            throw Error(ErrorCode::not_found, "no such local service: " + name);
        auto& service = found->second;
        if (service.record.kind != wire::DATA && service.record.kind != wire::RESEARCH)
            throw std::invalid_argument("service does not support native plugins");
        if (service.record.desired || service.process)
            throw std::invalid_argument("stop the service before configuring plugins");
        if (service.record.revision() != expected)
            throw std::invalid_argument("service configuration changed; inspect again");
        const auto selection =
            local_plugin_catalog().select_data_task_plugins(plugins, service.record.plugins);
        for (const auto& artifact : selection.uploads)
            impl.keep_plugin(artifact);
        auto next = service.record;
        next.plugins = selection.hashes;
        std::sort(next.plugins.begin(), next.plugins.end());
        impl.save(name, next);
        service.record = std::move(next);
        service.restarts = 0;
        service.error.clear();
        impl.publish();
    });
}

std::vector<HistoryService> LocalServices::history() const
{
    std::shared_ptr<const wire::Status> status;
    {
        std::lock_guard lock(impl_->mutex);
        status = impl_->published;
    }
    std::vector<HistoryService> result;
    if (!status)
        return result;
    for (const auto& service : status->services())
        if (service.kind() == wire::DATA || service.kind() == wire::RESEARCH)
            result.push_back(
                {service.kind(),
                    service.directory(),
                    service.state(),
                    ServiceEndpoint{
                        "",
                        service.id(),
                        0,
                        service.endpoint(),
                        "local",
                        status->node_id()
                    }});
    return result;
}

} // namespace asterion::terminal
