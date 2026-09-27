#include "runtime_info.hpp"

#include <iostream>

namespace asterion {
namespace {
class RuntimeInfo final : public Plugin {
public:
    PluginDescriptor descriptor() const override {
        return {"asterion.tool.runtime-info", PluginKind::tool, plugin_contract_version, {}};
    }
    void start() override {
        std::cout << "Asterion Core | C++20 | plugin contract " << plugin_contract_version << '\n';
    }
    void stop() noexcept override {}
};
}
std::unique_ptr<Plugin> make_runtime_info() { return std::make_unique<RuntimeInfo>(); }
}
