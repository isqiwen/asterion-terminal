#pragma once
#include <memory>
#include <nlohmann/json.hpp>
namespace asterion::terminal {
class Application {
public:
    Application();
    ~Application();
    nlohmann::json dispatch(const nlohmann::json& request);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
