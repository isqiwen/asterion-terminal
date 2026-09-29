#include "task_worker.hpp"
#include "engine.hpp"
#include <asterion/protocol/task_client.hpp>
namespace asterion::backtest {
int run_task(const std::string& endpoint, const std::string& host, unsigned short port,
             const ipc::TlsIdentity& tls, const std::string& service, const std::string& task) {
  return protocol::run_task_worker(endpoint, host, port, tls, service, task, research::v1::BACKTEST,
                                   [](const auto& input, auto stop, const auto& progress) {
                                     research::v1::TaskFinish result;
                                     *result.mutable_result() =
                                         run(input.task().input(), stop, progress);
                                     return result;
                                   });
}
} // namespace asterion::backtest
