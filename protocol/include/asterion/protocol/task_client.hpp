#pragma once
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <asterion/protocol/task.hpp>
#include <functional>
#include <stop_token>
namespace asterion::protocol {
using TaskProgress = std::function<void(std::size_t, std::size_t)>;
using TaskRunner = std::function<task::v1::TaskFinish(const task::v1::TaskAttempt&, std::stop_token,
                                                      const TaskProgress&)>;
int run_task_worker(const std::string& endpoint, const std::string& host, unsigned short port,
                    const ipc::TlsIdentity& tls, const std::string& service,
                    const std::string& task, task::v1::TaskKind kind, const TaskRunner& runner,
                    std::uint64_t owner_pid);
} // namespace asterion::protocol
