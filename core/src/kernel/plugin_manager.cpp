#include <asterion/kernel/plugin.hpp>

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace asterion {

PluginManager::~PluginManager() { stop(); }

void PluginManager::add(std::unique_ptr<Plugin> plugin) {
    if (running_ || transitioning_) {
        throw std::logic_error("cannot register plugins during an active lifecycle");
    }
    if (!plugin) {
        throw std::invalid_argument("null plugin");
    }
    auto descriptor = plugin->descriptor();
    if (descriptor.id.empty() || descriptor.contract_version != plugin_contract_version) {
        throw std::invalid_argument("invalid plugin identity or unsupported contract");
    }
    switch (descriptor.kind) {
    case PluginKind::data:
    case PluginKind::execution:
    case PluginKind::storage:
    case PluginKind::strategy:
    case PluginKind::risk:
    case PluginKind::tool:
    case PluginKind::ui:
        break;
    default:
        throw std::invalid_argument("unsupported plugin kind");
    }
    if (std::ranges::any_of(entries_, [&](const auto& entry) {
            return entry.descriptor.id == descriptor.id;
        })) {
        throw std::invalid_argument("duplicate plugin: " + descriptor.id);
    }
    std::unordered_set<std::string> dependencies;
    for (const auto& id : descriptor.dependencies) {
        if (id.empty() || id == descriptor.id || !dependencies.insert(id).second) {
            throw std::invalid_argument("invalid plugin dependency: " + descriptor.id);
        }
    }
    entries_.push_back({std::move(descriptor), std::move(plugin)});
}

void PluginManager::start() {
    if (running_ || transitioning_) {
        throw std::logic_error("plugin lifecycle already active");
    }
    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        index.emplace(entries_[i].descriptor.id, i);
    }
    std::vector<unsigned char> marks(entries_.size(), 0);
    std::vector<std::size_t> order;
    std::function<void(std::size_t)> visit = [&](std::size_t i) {
        if (marks[i] == 2) return;
        if (marks[i] == 1) {
            throw std::invalid_argument("cyclic plugin dependency: " + entries_[i].descriptor.id);
        }
        marks[i] = 1;
        for (const auto& dependency : entries_[i].descriptor.dependencies) {
            const auto found = index.find(dependency);
            if (found == index.end()) {
                throw std::invalid_argument("missing plugin dependency: " + dependency);
            }
            visit(found->second);
        }
        marks[i] = 2;
        order.push_back(i);
    };
    // Validate the complete graph before performing any plugin side effects.
    for (std::size_t i = 0; i < entries_.size(); ++i) visit(i);
    started_.reserve(order.size());
    transitioning_ = true;
    try {
        for (const auto i : order) {
            entries_[i].plugin->start();
            started_.push_back(i);
        }
        running_ = true;
        transitioning_ = false;
    } catch (...) {
        for (auto it = started_.rbegin(); it != started_.rend(); ++it) {
            entries_[*it].plugin->stop();
        }
        started_.clear();
        transitioning_ = false;
        throw;
    }
}

void PluginManager::stop() noexcept {
    if (transitioning_) return;
    transitioning_ = true;
    for (auto it = started_.rbegin(); it != started_.rend(); ++it) {
        entries_[*it].plugin->stop();
    }
    started_.clear();
    running_ = false;
    transitioning_ = false;
}

std::vector<PluginDescriptor> PluginManager::descriptors() const {
    std::vector<PluginDescriptor> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_) result.push_back(entry.descriptor);
    return result;
}

} // namespace asterion
