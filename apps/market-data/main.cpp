#include "ctp_feed.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/protocol/market.hpp>
#include <asterion/protocol/trading.hpp>
#include <atomic>
#include <csignal>
#include <iostream>
#include <mutex>
#include <thread>
using namespace asterion;
using namespace std::chrono_literals;
namespace wire = asterion::market::v1;
namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) {
  stopping = 1;
}
} // namespace
int main(int argc, char** argv) {
  CLI::App app{"Asterion read-only live market-data host"};
  app.set_version_flag("--version", "Asterion Market Data 0.1.0");
  std::string service, endpoint, health_endpoint, directory, bind, sdk;
  unsigned short port = 0;
  std::uint64_t owner_pid = 0;
  ipc::TlsIdentity tls;
  app.add_option("--session", service)->required();
  app.add_option("--directory", directory)->required()->check(CLI::ExistingDirectory);
  app.add_option("--endpoint", endpoint);
  app.add_option("--health-endpoint", health_endpoint);
  app.add_option("--bind", bind);
  app.add_option("--port", port);
  app.add_option("--tls-ca", tls.ca_file);
  app.add_option("--tls-cert", tls.certificate_file);
  app.add_option("--tls-key", tls.private_key_file);
  app.add_option("--ctp-library", sdk);
  app.add_option("--owner-pid", owner_pid);
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  try {
    validate_id(service);
    const bool remote = !bind.empty();
    if (remote ? (!endpoint.empty() || !port || tls.ca_file.empty() ||
                  tls.certificate_file.empty() || tls.private_key_file.empty())
               : (endpoint.empty() || port || !tls.ca_file.empty() ||
                  !tls.certificate_file.empty() || !tls.private_key_file.empty()))
      throw std::invalid_argument("choose local IPC or TCP with mutual TLS");
    const auto instance = unique_process_id();
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::unique_ptr<ProcessOwner> owner;
    if (owner_pid)
      owner = std::make_unique<ProcessOwner>(owner_pid);
    std::jthread owner_watch([&](std::stop_token token) {
      while (!token.stop_requested()) {
        if (owner && !owner->alive())
          std::_Exit(4);
        std::this_thread::sleep_for(200ms);
      }
    });
    PluginManager plugins;
    ctp::Feed* feed = nullptr;
    std::mutex mutex;
    auto snapshot = [&] {
      std::lock_guard lock(mutex);
      LiveMarketSnapshot state;
      if (feed)
        state = feed->snapshot();
      else
        state.phase = sdk.empty() ? "sdk_unavailable" : "disconnected";
      return protocol::encode_market(state, instance);
    };
    auto serve = [&](auto channel, std::stop_token token, bool health_only) {
      try {
        wire::Request request;
        if (!request.ParseFromString(channel.receive(2s)))
          return;
        protocol::validate_message(request);
        wire::Response response;
        response.set_version(1);
        response.set_service_id(service);
        response.set_correlation_id(request.correlation_id());
        try {
          validate_id(request.correlation_id());
          if (request.version() != 1 || request.service_id() != service)
            throw std::invalid_argument("market service identity mismatch");
          if (health_only && !request.has_heartbeat())
            throw std::invalid_argument("health channel accepts heartbeat only");
          if (request.has_heartbeat()) {
            auto* h = response.mutable_health();
            h->set_instance_id(instance);
            h->set_phase(snapshot().phase());
          } else if (request.has_events()) {
            std::lock_guard lock(mutex);
            if (!feed)
              throw Error(ErrorCode::unavailable, "market event stream not started");
            const auto& read = request.events();
            *response.mutable_events() = protocol::encode_market_events(
                feed->events_after(read.stream_id(), read.after_sequence(), read.limit()));
          } else if (request.has_watch()) {
            // Coalesced quote snapshots; intermediate ticks are not a
            // historical tick archive.
            while (!token.stop_requested() && !stopping) {
              *response.mutable_snapshot() = snapshot();
              channel.send(response.SerializeAsString(), 2s);
              std::this_thread::sleep_for(250ms);
            }
            return;
          } else {
            {
              std::lock_guard lock(mutex);
              if (request.has_connect()) {
                if (feed && feed->snapshot().phase != "disconnected" &&
                    feed->snapshot().phase != "error")
                  throw Error(ErrorCode::conflict, "disconnect current market session first");
                if (!feed) {
                  auto plugin = std::make_unique<ctp::Feed>(
                      std::filesystem::path(std::u8string(sdk.begin(), sdk.end())),
                      std::filesystem::path(std::u8string(directory.begin(), directory.end())) /
                          "ctp-flow");
                  feed = plugin.get();
                  plugins.add(std::move(plugin));
                  plugins.start();
                }
                auto* c = request.mutable_connect();
                std::vector<InstrumentId> ids;
                for (const auto& i : c->instruments())
                  ids.push_back({i.venue(), i.symbol()});
                ctp::Configuration config{c->front(), c->broker(), c->user(), c->password()};
                c->clear_password();
                feed->connect(std::move(config), ids);
              } else if (request.has_subscribe()) {
                if (!feed)
                  throw Error(ErrorCode::conflict, "connect market data first");
                std::vector<InstrumentId> ids;
                for (const auto& i : request.subscribe().instruments())
                  ids.push_back({i.venue(), i.symbol()});
                feed->subscribe(ids);
              } else if (request.has_disconnect()) {
                if (feed)
                  feed->disconnect();
              } else if (!request.has_snapshot())
                throw std::invalid_argument("missing market operation");
            }
            *response.mutable_snapshot() = snapshot();
          }
        } catch (const Error& e) {
          response.mutable_error()->set_code(std::string(error_name(e.code())));
          response.mutable_error()->set_message(e.what());
        } catch (const std::exception& e) {
          // Provider diagnostics may echo configuration; only the code crosses.
          response.mutable_error()->set_code(std::string(error_name(classify(e))));
          response.mutable_error()->set_message(
              "Market request rejected; check configuration and SDK "
              "availability");
        }
        channel.send(response.SerializeAsString(), 2s);
      } catch (const std::exception&) { /* Bounded peer failure never stops the managed feed. */
      }
    };
    std::unique_ptr<ipc::Listener> health;
    if (!health_endpoint.empty())
      health = std::make_unique<ipc::Listener>(health_endpoint);
    std::jthread health_thread([&](std::stop_token token) {
      while (health && !token.stop_requested() && !stopping) {
        try {
          serve(health->accept(200ms), token, true);
        } catch (const Error&) {
        }
      }
    });
    struct Worker {
      std::shared_ptr<std::atomic<bool>> done;
      std::jthread thread;
    };
    std::vector<Worker> workers;
    // Handshakes run on the per-client worker, never on the accept loop.
    auto launch = [&](auto connection) {
      std::erase_if(workers, [](const Worker& w) { return w.done->load(); });
      if (workers.size() >= 16)
        return;
      auto done = std::make_shared<std::atomic<bool>>(false);
      workers.push_back({done, std::jthread([&, done, connection = std::move(connection)](
                                                std::stop_token token) mutable {
                           try {
                             if constexpr (requires { std::move(connection).handshake(2s); })
                               serve(std::move(connection).handshake(2s), token, false);
                             else
                               serve(std::move(connection), token, false);
                           } catch (const std::exception&) {
                           }
                           done->store(true);
                         })});
    };
    if (remote) {
      ipc::TlsListener listener(bind, port, tls);
      while (!stopping) {
        try {
          launch(listener.accept_pending(200ms));
        } catch (const Error&) {
        }
      }
    } else {
      ipc::Listener listener(endpoint);
      while (!stopping) {
        try {
          launch(listener.accept(200ms));
        } catch (const Error&) {
        }
      }
    }
    for (auto& worker : workers)
      worker.thread.request_stop();
    for (auto& worker : workers)
      worker.thread.join();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
