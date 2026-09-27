#pragma once
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <asterion/protocol/research.hpp>
#include <functional>
#include <stop_token>
namespace asterion::protocol {
using TaskProgress = std::function<void(std::size_t, std::size_t)>;
using TaskRunner = std::function<research::v1::TaskFinish(
    const research::v1::Task &, std::stop_token, const TaskProgress &)>;
int run_task_worker(const std::string &endpoint, const std::string &host,
                    unsigned short port, const ipc::TlsIdentity &tls,
                    const std::string &service, const std::string &task,
                    research::v1::TaskKind kind, const TaskRunner &runner);
} // namespace asterion::protocol
