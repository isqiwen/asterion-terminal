#include "lab_supervisor.hpp"
#include "program_paths.hpp"
#include <asterion/base/error.hpp>
#include <asterion/base/id.hpp>
#include <asterion/lab.pb.h>
#include <asterion/system/durable_file.hpp>
#include <asterion/system/environment.hpp>
#include <asterion/system/ipc/rpc_client.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/file_lock.hpp>
#include <asterion/system/process/owner.hpp>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

namespace asterion::terminal {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

std::string utf8(const fs::path& path)
{
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

// One heartbeat on Lab's private health endpoint, bounded to 1.5 s: whether it
// has finished starting.
bool heartbeat(const std::string& endpoint)
{
    ipc::RpcClient channel(endpoint, 1, PayloadBudget{256 * 1024}, 64 * 1024);
    ::lab::Request ping;
    ping.set_id(1);
    ping.mutable_get_health();
    auto pending = channel.request(ping.SerializeAsString(), 1500ms);
    while (pending.wait_for(0ms) != std::future_status::ready) {
        channel.poll();
        std::this_thread::sleep_for(1ms);
    }
    ::lab::Response reply;
    if (!reply.ParseFromString(*pending.get()) || !reply.has_get_health())
        throw std::runtime_error("invalid lab health");
    return reply.get_health().ready();
}

} // namespace

struct LabSupervisor::Impl
{
    std::function<std::optional<fs::path>()> python;
    std::unique_ptr<FileLock> owner;
    const std::string instance = unique_process_id();
    fs::path sockets;

    // Owned by the supervisor thread.
    bool wanted = false, answered = false, ready = false, unresponsive = false;
    std::unique_ptr<ChildProcess> process;
    Clock::time_point started{}, probe{}, retry{};
    unsigned failures = 0, restarts = 0;
    std::string error;
    // Callers of serving() waiting for Lab to answer its first heartbeat.
    std::vector<std::promise<ServiceEndpoint>> waiting;

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> work;
    Json published = nullptr;
    bool stopping = false;
    std::jthread supervisor;

    std::string endpoint() const
    {
        return utf8(sockets / "lab.sock");
    }

    std::string health_endpoint() const
    {
        return utf8(sockets / "lab.health");
    }

    // Lab's directories and logging defaults, and the built-in strategies.
    std::unique_ptr<ChildProcess> launch() const
    {
        if (const auto builtin = environment_path("ASTERION_BUILTIN_RESEARCH"))
            sync_builtin_research(*builtin, asterion_root() / "research");
        const auto folder = program_directory("lab");
        for (const auto* part : {"config", "logs", "data"})
            create_directories_durably(folder / part);
        if (const auto logging = folder / "config" / "logging.json"; !fs::exists(logging)) {
            const auto source = configuration_templates() / "logging.json";
            std::ifstream input(source, std::ios::binary);
            if (!input)
                throw std::runtime_error("cannot read configuration default: " + source.string());
            replace_file_durably(logging, std::string{std::istreambuf_iterator<char>(input), {}});
        }
        const auto interpreter = python();
        if (!interpreter || !fs::is_regular_file(*interpreter))
            throw Error(
                ErrorCode::unavailable,
                "lab needs the Python runtime; it is installed at the first launch");
        fs::remove(endpoint());
        fs::remove(health_endpoint());
        const auto directory = folder / "data" / "lab";
        create_directories_durably(directory);
        // Isolated from the user's environment and site; no bytecode written.
        return std::make_unique<ChildProcess>(
            *interpreter,
            std::vector<std::string>{
                "-I",
                "-B",
                "-m",
                "asterion_lab",
                "--owner-pid",
                std::to_string(current_process_id()),
                "--session",
                "lab",
                "--directory",
                utf8(directory),
                "--terminal-data",
                utf8(program_directory("terminal") / "data"),
                "--health-endpoint",
                health_endpoint(),
                "--endpoint",
                endpoint()
            });
    }

    void start(bool automatic)
    {
        if (automatic)
            ++restarts;
        process.reset();
        try {
            process = launch();
            log::info("lab started: pid={}", process->id());
            answered = ready = unresponsive = false;
            failures = 0;
            started = Clock::now();
            probe = started + 200ms;
            error.clear();
        } catch (const std::exception& failure) {
            process.reset();
            error = failure.what();
            log::error("lab did not start: {}", failure.what());
        }
        retry = Clock::now() + 5s;
    }

    void stop()
    {
        if (process) {
            process->request_stop();
            process.reset();
        }
    }

    void supervise()
    {
        if (!wanted)
            return;
        if (process && process->exited()) {
            log::warn("lab exited: exit_code={}", process->exit_code());
            process.reset();
            error = "process exited; restarting";
        }
        if (process && Clock::now() >= probe) {
            probe = Clock::now() + 5s;
            try {
                ready = heartbeat(health_endpoint());
                if (!ready)
                    probe = Clock::now() + 250ms;
                answered = true;
                unresponsive = false;
                failures = 0;
                error.clear();
            } catch (const std::exception& failure) {
                if (!answered && Clock::now() - started < 10s) {
                    probe = Clock::now() + 250ms;
                    error = std::string("waiting for the first heartbeat: ") + failure.what();
                } else {
                    unresponsive = true;
                    error = "lab heartbeat unavailable";
                    log::warn(
                        "lab heartbeat failed ({} in a row): {}",
                        failures + 1,
                        failure.what());
                    if (++failures >= 3) {
                        stop();
                        retry = Clock::now() + 5s;
                    }
                }
            }
        }
        if (!process && Clock::now() >= retry)
            start(true);
    }

    // Lab is usable once it answers a heartbeat; until then its socket may not
    // exist yet. Waiting callers fail when it stops running or never answers.
    void answer_waiting()
    {
        if (waiting.empty())
            return;
        const bool failed = !process || unresponsive;
        if (!failed && !answered)
            return;
        for (auto& promise : waiting) {
            if (failed)
                promise.set_exception(
                    std::make_exception_ptr(Error(
                        ErrorCode::unavailable,
                        error.empty() ? "lab did not start" : error)));
            else
                promise.set_value(ServiceEndpoint{"", "lab", 0, endpoint()});
        }
        waiting.clear();
    }

    Json report() const
    {
        if (!wanted)
            return nullptr;
        return {
            {"state",
                !process           ? "restarting"
                    : unresponsive ? "unresponsive"
                    : ready        ? "ready"
                                   : "starting"},
            {"pid", process ? process->id() : 0},
            {"restarts", restarts},
            {"error", error}
        };
    }

    void publish()
    {
        auto next = report();
        std::lock_guard lock(mutex);
        published = std::move(next);
    }

    // Runs `task` on the supervisor thread.
    void post(std::function<void()> task)
    {
        {
            std::lock_guard lock(mutex);
            if (stopping)
                throw Error(ErrorCode::unavailable, "lab supervisor is stopping");
            work.push_back(std::move(task));
        }
        wake.notify_one();
    }

    void run()
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
            supervise();
            answer_waiting();
            publish();
        }
        stop();
        error = "the Terminal is closing";
        answer_waiting();
    }

    explicit Impl(std::function<std::optional<fs::path>()> interpreter)
        : python(std::move(interpreter))
    {
        const auto state = program_directory("terminal") / "data";
        create_directories_durably(state);
        owner = std::make_unique<FileLock>(state, "services.lock");
        // Short and private: socket paths have a small length limit.
        sockets = fs::path("/tmp") / ("ast-" + instance.substr(0, 12));
        if (!fs::create_directory(sockets))
            throw std::runtime_error("cannot create the private service socket directory");
        fs::permissions(sockets, fs::perms::owner_all, fs::perm_options::replace);
        supervisor = std::jthread([this] {
            run();
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
};

LabSupervisor::LabSupervisor(std::function<std::optional<fs::path>()> python)
    : impl_(std::make_unique<Impl>(std::move(python)))
{}

LabSupervisor::~LabSupervisor() = default;

Json LabSupervisor::view() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->published;
}

std::future<ServiceEndpoint> LabSupervisor::serving()
{
    auto promise = std::make_shared<std::promise<ServiceEndpoint>>();
    auto result = promise->get_future();
    impl_->post([impl = impl_.get(), promise] {
        impl->wanted = true;
        if (!impl->process) {
            impl->restarts = 0;
            impl->start(false);
        }
        impl->waiting.push_back(std::move(*promise));
        impl->answer_waiting();
        impl->publish();
    });
    return result;
}

} // namespace asterion::terminal
