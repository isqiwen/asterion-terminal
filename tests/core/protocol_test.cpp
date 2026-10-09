#include <gtest/gtest.h>
#include "support/local_listener.hpp"
#include <asterion/protocol/trading.hpp>
#include <asterion/protocol/health.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <future>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <fstream>
#include <cstdlib>
using namespace asterion;
using namespace std::chrono_literals;
namespace wire = asterion::protocol::v1;
namespace {
struct Endpoint {
  std::string path;
  std::filesystem::path directory;
  Endpoint() {
    directory = std::filesystem::path("/tmp") / ("ast-test-" + unique_process_id());
    std::filesystem::create_directory(directory);
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
    path = (directory / "socket").string();
  }
  ~Endpoint() {
    if (!directory.empty()) {
      std::error_code ignored;
      std::filesystem::remove(directory, ignored);
    }
  }
};
} // namespace
TEST(Protobuf, TypedDecimalAndCommandRoundTrip) {
  Json command{{"request_id", "r1"},      {"action", "submit"}, {"order_id", "o1"},
               {"venue", "SHFE"},         {"symbol", "rb2610"}, {"side", "sell"},
               {"offset", "close_today"}, {"quantity", "2"},    {"price", "92233720368.54775807"}};
  auto message = protocol::encode_command(command);
  EXPECT_EQ(message.submit().price().units(), INT64_MAX);
  wire::Command parsed;
  ASSERT_TRUE(parsed.ParseFromString(message.SerializeAsString()));
  EXPECT_EQ(protocol::decode_command(parsed), command);
  auto unknown = message.SerializeAsString();
  unknown.append("\xf8\x07\x01", 3);
  ASSERT_TRUE(parsed.ParseFromString(unknown));
  EXPECT_THROW(protocol::validate_message(parsed), std::invalid_argument);
  auto wrong = command;
  wrong["offset"] = "auto";
  EXPECT_THROW(protocol::encode_command(wrong), std::invalid_argument);
  wire::Command absent;
  EXPECT_THROW(protocol::decode_command(absent), std::invalid_argument);
}
TEST(Protobuf, UnknownFieldsInsideOneofAndRepeatedMessagesAreRejected) {
  wire::Request request;
  auto* price = request.mutable_command()->mutable_submit()->mutable_price();
  price->GetReflection()->MutableUnknownFields(price)->AddVarint(127, 1);
  EXPECT_THROW(protocol::validate_message(request), std::invalid_argument);
  request.clear_command();
  request.mutable_snapshot();
  EXPECT_NO_THROW(protocol::validate_message(request));
  wire::Snapshot snapshot;
  snapshot.add_fills()->mutable_price()->set_units(10);
  auto* second = snapshot.add_fills()->mutable_quantity();
  second->GetReflection()->MutableUnknownFields(second)->AddVarint(127, 1);
  EXPECT_THROW(protocol::validate_message(snapshot), std::invalid_argument);
  second->GetReflection()->MutableUnknownFields(second)->Clear();
  EXPECT_NO_THROW(protocol::validate_message(snapshot));
}
TEST(LocalIpc, BoundedBinaryFramesAndDisconnect) {
  Endpoint endpoint;
  testing_support::LocalListener listener(endpoint.path);
  auto server = std::async(std::launch::async, [&] {
    auto peer = listener.accept(2s);
    auto bytes = peer.receive(2s);
    EXPECT_EQ(bytes, std::string(200000, '\0') + "tail");
    peer.send("ok", 2s);
  });
  auto client = ipc::Channel::connect(endpoint.path, 2s);
  EXPECT_THROW(client.send(std::string(ipc::Channel::max_frame + 1, 'x'), 2s), Error);
  client.send(std::string(200000, '\0') + "tail", 2s);
  EXPECT_EQ(client.receive(2s), "ok");
  server.get();
  EXPECT_THROW(client.receive(100ms), Error);
}
TEST(LocalIpc, DeadlineClosesTheChannel) {
  Endpoint endpoint;
  testing_support::LocalListener listener(endpoint.path);
  auto client = ipc::Channel::connect(endpoint.path, 2s);
  auto server = listener.accept(2s);
  EXPECT_THROW(client.receive(20ms), Error);
  EXPECT_THROW(client.send("late", 100ms), Error);
  EXPECT_THROW(server.receive(100ms), Error);
}

TEST(Protocol, RpcDiagnosticsNeverSerializeCredentialsOrRemoteErrorMessages) {
  const auto root =
      std::filesystem::temp_directory_path() / ("asterion-rpc-log-" + unique_process_id());
  const char* value = std::getenv("ASTERION_LOG_DIRECTORY");
  const std::optional<std::string> saved = value ? std::optional<std::string>(value) : std::nullopt;
  struct Restore {
    const std::optional<std::string>& saved;
    std::filesystem::path root;
    ~Restore() {
      if (saved)
        ::setenv("ASTERION_LOG_DIRECTORY", saved->c_str(), 1);
      else
        ::unsetenv("ASTERION_LOG_DIRECTORY");
      std::error_code ignored;
      std::filesystem::remove_all(root, ignored);
    }
  } restore{saved, root};
  ASSERT_EQ(::setenv("ASTERION_LOG_DIRECTORY", root.c_str(), 1), 0);
  wire::Request request;
  request.set_correlation_id("rpc.test");
  request.mutable_live_connect()->set_password("test-only-password");
  request.mutable_live_connect()->set_auth_code("test-only-auth");
  wire::Response response;
  response.mutable_error()->set_code("permission_denied");
  response.mutable_error()->set_message("test-only-private-provider-diagnostic");
  protocol::log_rpc_result("rpc-test", request, response, true, {{"session_id", "session.test"}});
  std::vector<Json> records;
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    std::ifstream stream(entry.path());
    std::string line;
    while (std::getline(stream, line)) {
      EXPECT_EQ(line.find("test-only"), std::string::npos);
      records.push_back(Json::parse(line));
    }
  }
  ASSERT_EQ(records.size(), 1U);
  const auto& fields = records.front().at("fields");
  EXPECT_EQ(fields, (Json{{"success", false},
                          {"correlation_id", "rpc.test"},
                          {"operation", "live_connect"},
                          {"code", "permission_denied"},
                          {"session_id", "session.test"}}));
}

TEST(Protobuf, TradingHealthSeparatesReadinessProgressAndRecovery) {
  wire::Health h;
  EXPECT_THROW(protocol::trading_health_phase(h), std::invalid_argument);
  auto* e = h.mutable_execution();
  EXPECT_EQ(protocol::trading_health_phase(h), protocol::ServiceHealth::awaiting_input);
  e->mutable_initialization()->set_observed(true);
  e->mutable_initialization()->set_pending(true);
  EXPECT_EQ(protocol::trading_health_phase(h), protocol::ServiceHealth::starting);
  e->mutable_initialization()->set_pending(false);
  h.set_initialized(true);
  for (auto* p : {e->mutable_io(), e->mutable_state(), e->mutable_persistence()})
    p->set_observed(true);
  EXPECT_EQ(protocol::trading_health_phase(h), protocol::ServiceHealth::awaiting_input);
  e->set_business_ready(true);
  e->mutable_persistence()->set_age_ms(60000); // Idle writer needs no artificial work.
  EXPECT_EQ(protocol::trading_health_phase(h), protocol::ServiceHealth::ready);
  e->mutable_persistence()->set_pending(true);
  EXPECT_EQ(protocol::trading_health_phase(h), protocol::ServiceHealth::degraded);
  EXPECT_FALSE(h.recovery_required());
  e->mutable_persistence()->set_pending(false);
  protocol::age_execution_health(*e, 31000); // Agent cannot renew executor progress.
  EXPECT_EQ(e->state().age_ms(), 31000U);
  EXPECT_EQ(e->persistence().age_ms(), 91000U);
  EXPECT_FALSE(e->command().observed());
  EXPECT_EQ(e->command().age_ms(), 0U);
  EXPECT_EQ(protocol::trading_health_phase(h), protocol::ServiceHealth::degraded);
  e->mutable_io()->set_age_ms(0);
  e->mutable_state()->set_age_ms(0);
  h.set_recovery_required(true);
  EXPECT_EQ(protocol::trading_health_phase(h), protocol::ServiceHealth::degraded);
}
