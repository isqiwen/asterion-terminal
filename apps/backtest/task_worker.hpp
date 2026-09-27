#pragma once
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <string>
namespace asterion::backtest {
int run_task(const std::string& endpoint, const std::string& host, unsigned short port,
             const ipc::TlsIdentity& tls, const std::string& service, const std::string& task);
}
