#include "python_runtime.hpp"
#include <asterion/base/error.hpp>
#include <asterion/system/durable_file.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/system/process/child.hpp>
#include <httplib.h>
#include <fstream>
#include <set>
#include <stop_token>

namespace asterion::terminal {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::string read(const fs::path& file)
{
    std::ifstream input(file, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read " + file.string());
    return {std::istreambuf_iterator<char>(input), {}};
}

// Runs a program to its end; its output goes to its own log.
void run_program(
    const fs::path& executable,
    const std::vector<std::string>& arguments,
    const fs::path& log,
    std::stop_token stop)
{
    fs::remove(log);
    ChildProcess child(executable, arguments, false, log, true);
    while (!child.wait(200ms))
        if (stop.stop_requested()) {
            child.request_stop();
            throw std::runtime_error("runtime installation cancelled");
        }
    if (child.exit_code() != 0)
        throw std::runtime_error(executable.filename().string() + " failed; see " + log.string());
}

} // namespace

PythonRuntime::PythonRuntime(fs::path application, fs::path directory)
    : application_(std::move(application)),
      directory_(std::move(directory))
{
    const auto file = application_ / "manifest.json";
    if (!fs::exists(file)) {
        state_ = "missing";
        error_ = "the application ships no runtime manifest: " + file.string();
        return;
    }
    const auto text = read(file);
    identity_ = sha256_bytes(text);
    const auto manifest = parse_json(text, 1 << 20);
    const auto& interpreter = manifest.at("interpreter");
    Module first{.id = "interpreter", .title = "Python 解释器"};
    File archive{
        .name = "interpreter",
        .sha256 = interpreter.at("sha256").get<std::string>(),
        .size = interpreter.at("size").get<std::uint64_t>()
    };
    const auto path = interpreter.at("path").get<std::string>();
    for (const auto* source : {"mirror", "official"})
        archive.urls.push_back(interpreter.at("sources").at(source).get<std::string>() + path);
    first.files.push_back(std::move(archive));
    modules_.push_back(std::move(first));
    const auto& sources = manifest.at("packages");
    for (const auto& item : manifest.at("modules")) {
        Module module{
            .id = item.at("id").get<std::string>(),
            .title = item.at("title").get<std::string>()
        };
        for (const auto& entry : item.at("files")) {
            File file{
                .name = entry.at("file").get<std::string>(),
                .sha256 = entry.at("sha256").get<std::string>(),
                .size = entry.at("size").get<std::uint64_t>()
            };
            if (entry.value("bundled", false))
                file.bundled = application_ / "wheels" / file.name;
            else {
                const auto at = entry.at("path").get<std::string>();
                for (const auto& mirror : sources.at("mirrors"))
                    file.urls.push_back(mirror.get<std::string>() + at);
                file.urls.push_back(sources.at("official").get<std::string>() + at);
            }
            module.files.push_back(std::move(file));
        }
        modules_.push_back(std::move(module));
    }
    for (auto& module : modules_)
        for (const auto& file : module.files)
            module.size += file.size;
    state_ = installed() ? "ready" : "missing";
    if (state_ == "ready") {
        for (auto& module : modules_) {
            module.state = "done";
            module.done = module.size;
        }
        link_application();
    }
}

PythonRuntime::~PythonRuntime() = default;

bool PythonRuntime::installed() const
{
    const auto marker = directory_ / "python" / "asterion-runtime";
    return !identity_.empty() && fs::exists(marker) && read(marker) == identity_;
}

std::optional<fs::path> PythonRuntime::python() const
{
    std::lock_guard lock(mutex_);
    if (state_ != "ready")
        return std::nullopt;
    return directory_ / "python" / "bin" / "python3";
}

Json PythonRuntime::status() const
{
    std::lock_guard lock(mutex_);
    Json modules = Json::array();
    for (const auto& module : modules_)
        modules.push_back(
            {{"id", module.id},
                {"title", module.title},
                {"size", module.size},
                {"done", module.done},
                {"state", module.state}});
    return {{"state", state_}, {"error", error_}, {"modules", modules}};
}

void PythonRuntime::install()
{
    std::lock_guard lock(mutex_);
    if (state_ == "ready" || state_ == "installing" || identity_.empty())
        return;
    state_ = "installing";
    error_.clear();
    for (auto& module : modules_)
        if (module.state != "done") {
            module.state = "waiting";
            module.done = 0;
        }
    worker_ = std::jthread([this](std::stop_token stop) {
        try {
            run(stop);
            std::lock_guard done(mutex_);
            state_ = "ready";
        } catch (const std::exception& error) {
            log::error("python runtime installation failed: {}", error.what());
            std::lock_guard failed(mutex_);
            state_ = "failed";
            error_ = error.what();
            for (auto& module : modules_)
                if (module.state != "done")
                    module.state = "failed";
        }
    });
}

// One verified file: from the download cache, or downloaded into it from the
// first source that answers, resuming a partial download.
fs::path PythonRuntime::fetch(const File& file, Module& module, std::stop_token stop)
{
    // What this file adds to its module's progress; withdrawn when an attempt fails.
    std::uint64_t counted = 0;
    const auto count = [&](std::uint64_t total) {
        std::lock_guard lock(mutex_);
        module.done = module.done - counted + total;
        counted = total;
    };
    if (!file.bundled.empty()) {
        if (sha256_file(file.bundled) != file.sha256)
            throw std::runtime_error("the application's own " + file.name + " is damaged");
        count(file.size);
        return file.bundled;
    }
    // Kept under its digest and its own name, which pip reads a wheel by.
    const auto cache = directory_ / "downloads" / file.sha256;
    create_directories_durably(cache);
    const auto target = cache / file.name;
    if (fs::exists(target) && fs::file_size(target) == file.size) {
        count(file.size);
        return target;
    }
    const auto partial = cache / (file.name + ".part");
    std::string failures;
    for (const auto& url : file.urls) {
        if (stop.stop_requested())
            throw std::runtime_error("runtime installation cancelled");
        const auto slash = url.find('/', url.find("://") + 3);
        httplib::Client client(url.substr(0, slash));
        client.enable_server_certificate_verification(true);
        client.set_follow_location(true);
        client.set_connection_timeout(10);
        client.set_read_timeout(30);
        std::stop_callback cancel(stop, [&] {
            client.stop();
        });
        std::uint64_t received = fs::exists(partial) ? fs::file_size(partial) : 0;
        count(received);
        httplib::Headers headers;
        if (received)
            headers.emplace("Range", "bytes=" + std::to_string(received) + "-");
        std::ofstream output;
        const auto response = client.Get(
            url.substr(slash),
            headers,
            [&](const httplib::Response& reply) {
                if (reply.status != 200 && reply.status != 206)
                    return false;
                // A source that ignores the range sends the whole file again.
                if (reply.status == 200)
                    received = 0;
                output.open(
                    partial,
                    std::ios::binary | (received ? std::ios::app : std::ios::trunc));
                count(received);
                return output.good();
            },
            [&](const char* bytes, std::size_t size) {
                if (received + size > file.size)
                    return false;
                output.write(bytes, static_cast<std::streamsize>(size));
                received += size;
                count(received);
                return output.good() && !stop.stop_requested();
            });
        output.close();
        if (received == file.size) {
            if (sha256_file(partial) == file.sha256) {
                fs::rename(partial, target);
                sync_directory(cache);
                return target;
            }
            // Complete but wrong: never resumed from.
            fs::remove(partial);
        }
        count(0);
        failures += "\n  " + url + ": "
            + (response ? "HTTP " + std::to_string(response->status)
                        : httplib::to_string(response.error()));
    }
    throw std::runtime_error("cannot download " + file.name + failures);
}

void PythonRuntime::run(std::stop_token stop)
{
    std::vector<fs::path> archives;
    std::vector<fs::path> wheels;
    for (auto& module : modules_) {
        {
            std::lock_guard lock(mutex_);
            module.state = "downloading";
            module.done = 0;
        }
        for (const auto& file : module.files)
            (module.id == "interpreter" ? archives : wheels).push_back(fetch(file, module, stop));
        std::lock_guard lock(mutex_);
        module.state = "installing";
    }
    // Built aside and swapped in whole.
    const auto staging = directory_ / ".staging";
    fs::remove_all(staging);
    create_directories_durably(staging);
    run_program(
        "/usr/bin/tar",
        {"-xzf", archives.front().string(), "-C", staging.string()},
        directory_ / "extract.log",
        stop);
    std::vector<std::string> pip{
        "-I",
        "-m",
        "pip",
        "install",
        "--no-index",
        "--no-deps",
        "--disable-pip-version-check",
        "--no-warn-script-location"
    };
    for (const auto& wheel : wheels)
        pip.push_back(wheel.string());
    run_program(staging / "python" / "bin" / "python3", pip, directory_ / "install.log", stop);
    replace_file_durably(staging / "python" / "asterion-runtime", identity_);
    const auto current = directory_ / "python";
    const auto previous = directory_ / ".previous";
    fs::remove_all(previous);
    if (fs::exists(current))
        fs::rename(current, previous);
    fs::rename(staging / "python", current);
    sync_directory(directory_);
    fs::remove_all(previous);
    fs::remove_all(staging);
    // Downloads no runtime of this application needs any more.
    std::set<std::string> kept;
    for (const auto& module : modules_)
        for (const auto& file : module.files)
            kept.insert(file.sha256);
    for (const auto& entry : fs::directory_iterator(directory_ / "downloads"))
        if (!kept.contains(entry.path().filename().string()))
            fs::remove_all(entry.path());
    link_application();
    std::lock_guard lock(mutex_);
    for (auto& module : modules_) {
        module.state = "done";
        module.done = module.size;
    }
}

// The application's own Python code (lab, its protocol types, the backtest
// engine, the analytics helper) is imported from where the application is,
// which moves with every update.
void PythonRuntime::link_application() const
{
    const auto site = directory_ / "python" / "lib" / "python3.12" / "site-packages";
    const auto link = site / "asterion-application.pth";
    const auto code = fs::absolute(application_ / "app").string() + "\n";
    if (!fs::exists(link) || read(link) != code)
        replace_file_durably(link, code);
}

} // namespace asterion::terminal
