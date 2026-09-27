#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace asterion {

inline constexpr std::uint32_t plugin_contract_version = 1;

enum class PluginKind { data, execution, storage, strategy, risk, tool, ui };

struct PluginDescriptor {
    std::string id;
    PluginKind kind;
    std::uint32_t contract_version;
    std::vector<std::string> dependencies;
};

// Same-toolchain, trusted, in-process interface. This is not a dynamic-library ABI.
class Plugin {
public:
    virtual ~Plugin() = default;
    [[nodiscard]] virtual PluginDescriptor descriptor() const = 0;
    // A failed start must release its own partially acquired resources.
    virtual void start() = 0;
    virtual void stop() noexcept = 0;
};

// Single-threaded lifecycle owner. Plugins cannot be added while running.
class PluginManager final {
public:
    PluginManager() = default;
    ~PluginManager();
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    void add(std::unique_ptr<Plugin> plugin);
    void start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept { return running_; }
    [[nodiscard]] std::vector<PluginDescriptor> descriptors() const;

private:
    struct Entry {
        PluginDescriptor descriptor;
        std::unique_ptr<Plugin> plugin;
    };
    std::vector<Entry> entries_;
    std::vector<std::size_t> started_;
    bool running_ = false;
    bool transitioning_ = false;
};

} // namespace asterion
