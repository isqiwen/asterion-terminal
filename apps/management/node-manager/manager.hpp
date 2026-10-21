#pragma once
#include <asterion/kernel/rpc_host.hpp>
#include <memory>

namespace asterion::terminal {

class NodeManager
{
public:
    NodeManager();
    ~NodeManager();
    void open();
    service::RpcHost::Reply accept(std::string frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace asterion::terminal
