#include <gtest/gtest.h>
#include <asterion/kernel/plugin.hpp>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <utility>

using namespace asterion;

namespace {
class Probe final : public Plugin {
public:
    Probe(std::string id, std::vector<std::string> dependencies,
          std::vector<std::string>& events, bool fail = false,
          std::uint32_t version = plugin_contract_version)
        : descriptor_{std::move(id), PluginKind::tool, version, std::move(dependencies)},
          events_(events), fail_(fail) {}
    PluginDescriptor descriptor() const override { return descriptor_; }
    void start() override {
        events_.push_back("+" + descriptor_.id);
        if (fail_) throw std::runtime_error("start failed");
    }
    void stop() noexcept override { events_.push_back("-" + descriptor_.id); }
private:
    PluginDescriptor descriptor_;
    std::vector<std::string>& events_;
    bool fail_;
};

TEST(PluginManager, ordered_lifecycle) {
    std::vector<std::string> events;
    {
        PluginManager manager;
        manager.add(std::make_unique<Probe>("consumer", std::vector<std::string>{"source"}, events));
        manager.add(std::make_unique<Probe>("source", std::vector<std::string>{}, events));
        manager.start();
        EXPECT_TRUE((manager.running())) << "host should run";
        EXPECT_TRUE((events == std::vector<std::string>{"+source", "+consumer"})) << "dependency order";
        EXPECT_THROW(([&] { manager.start(); })(), std::logic_error);
        EXPECT_THROW(([&] {
            manager.add(std::make_unique<Probe>("late", std::vector<std::string>{}, events));
        })(), std::logic_error);
        manager.stop();
        manager.stop();
        EXPECT_TRUE((events == std::vector<std::string>{"+source", "+consumer", "-consumer", "-source"})) << "reverse stop once";
        manager.start();
    }
    EXPECT_TRUE((events.size() == 8 && events.back() == "-source")) << "destructor stops restarted plugins";
}

TEST(PluginManager, invalid_graphs) {
    for (bool cycle : {false, true}) {
        std::vector<std::string> events;
        PluginManager manager;
        manager.add(std::make_unique<Probe>("independent", std::vector<std::string>{}, events));
        manager.add(std::make_unique<Probe>("a", std::vector<std::string>{"b"}, events));
        if (cycle) manager.add(std::make_unique<Probe>("b", std::vector<std::string>{"a"}, events));
        EXPECT_THROW(([&] { manager.start(); })(), std::invalid_argument);
        EXPECT_TRUE((events.empty() && !manager.running())) << "invalid graph must have no side effects";
    }
}

TEST(PluginManager, failed_start) {
    std::vector<std::string> events;
    PluginManager manager;
    manager.add(std::make_unique<Probe>("source", std::vector<std::string>{}, events));
    manager.add(std::make_unique<Probe>("broken", std::vector<std::string>{"source"}, events, true));
    EXPECT_THROW(([&] { manager.start(); })(), std::runtime_error);
    EXPECT_TRUE((!manager.running())) << "rollback resets running state";
    manager.stop();
    EXPECT_TRUE((events == std::vector<std::string>{"+source", "+broken", "-source"})) << "rollback stops successful plugins exactly once";
    EXPECT_THROW(([&] { manager.start(); })(), std::runtime_error);
    EXPECT_TRUE((events.size() == 6)) << "failed start can be retried without stale state";
}

TEST(PluginManager, registration_validation) {
    std::vector<std::string> events;
    PluginManager manager;
    EXPECT_THROW(([&] { manager.add(nullptr); })(), std::invalid_argument);
    EXPECT_THROW(([&] {
        manager.add(std::make_unique<Probe>("", std::vector<std::string>{}, events));
    })(), std::invalid_argument);
    EXPECT_THROW(([&] {
        manager.add(std::make_unique<Probe>("version", std::vector<std::string>{}, events, false, 99));
    })(), std::invalid_argument);
    for (const auto& dependencies : std::vector<std::vector<std::string>>{{"self"}, {""}, {"x", "x"}}) {
        EXPECT_THROW(([&] {
            manager.add(std::make_unique<Probe>("self", dependencies, events));
        })(), std::invalid_argument);
    }
    manager.add(std::make_unique<Probe>("unique", std::vector<std::string>{}, events));
    EXPECT_THROW(([&] {
        manager.add(std::make_unique<Probe>("unique", std::vector<std::string>{}, events));
    })(), std::invalid_argument);
    EXPECT_TRUE((manager.descriptors().size() == 1)) << "rejected registrations leave registry unchanged";
}
}

