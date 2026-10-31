#pragma once
#include <asterion/base/serialization.hpp>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace asterion::terminal {

// The Python runtime lab runs on, installed at the Terminal's first launch
// from the manifest the application ships (scripts/desktop/runtime_manifest.py):
// the interpreter archive and every wheel, each downloaded from a mirror or
// the official source and checked against its SHA-256 before anything is
// installed. The runtime lives in <root>/terminal/runtime/python and is
// replaced whole: a failed or interrupted install leaves the previous one,
// or none. Verified downloads are kept, so a retry or an upgrade downloads
// only what it lacks.
class PythonRuntime
{
public:
    // `application`: the shipped runtime directory with manifest.json, the
    // wheels it bundles and app/, the application's own Python code.
    PythonRuntime(std::filesystem::path application, std::filesystem::path directory);
    ~PythonRuntime();

    // The interpreter, once the shipped manifest's runtime is installed.
    std::optional<std::filesystem::path> python() const;

    // {state: missing|installing|ready|failed, error, modules: [{id, title,
    // size, done, state}]}; each module's state is waiting, downloading,
    // installing, done or failed.
    Json status() const;

    // Starts installing in the background unless it is installed or under way.
    void install();

private:
    struct File
    {
        std::string name, sha256;
        std::uint64_t size = 0;
        std::vector<std::string> urls; // in the order to try
        std::filesystem::path bundled; // shipped with the application instead
    };

    struct Module
    {
        std::string id, title, state = "waiting";
        std::vector<File> files;
        std::uint64_t size = 0, done = 0;
    };

    void run(std::stop_token stop);
    std::filesystem::path fetch(const File& file, Module& module, std::stop_token stop);
    void link_application() const;
    bool installed() const;

    std::filesystem::path application_, directory_;
    std::string identity_; // SHA-256 of the shipped manifest
    mutable std::mutex mutex_;
    std::vector<Module> modules_;
    std::string state_, error_;
    std::jthread worker_;
};

} // namespace asterion::terminal
