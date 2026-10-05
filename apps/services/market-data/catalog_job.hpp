#pragma once
#include "ctp_catalog.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/thread_pool.hpp>
#include <asterion/protocol/market.hpp>
#include <asterion/protocol/trading.hpp>
#include <filesystem>
#include <fstream>
namespace asterion::market_data {
// State publication belongs to the market owner. Provider steps and cache I/O
// share its single SDK executor; only one step per operation can be outstanding.
class CatalogJob {
  using State = market::v1::CatalogState;
  struct Operation {
    std::filesystem::path file, library, flow;
    ctp::CatalogConfiguration config;
    bool load_cache = false;
    std::stop_source stop;
    std::unique_ptr<ctp::CatalogQuery> query;
    std::optional<State> result;
    void advance() {
      try {
        if (load_cache) {
          State cached;
          cached.set_phase("unconfigured");
          const auto status = std::filesystem::symlink_status(file);
          if (status.type() != std::filesystem::file_type::not_found) {
            if (!std::filesystem::is_regular_file(status) ||
                std::filesystem::file_size(file) > 64 * 1024 * 1024)
              throw Error(ErrorCode::invalid_request, "invalid cached CTP catalog");
            std::ifstream input(file, std::ios::binary);
            if (!cached.ParseFromIstream(&input) || cached.phase() != "ready" ||
                cached.contracts_size() > 20000)
              throw Error(ErrorCode::invalid_request, "invalid cached CTP catalog");
            protocol::validate_message(cached);
            cached.set_phase("cached");
          }
          result = std::move(cached);
          return;
        }
        if (stop.stop_requested())
          throw Error(ErrorCode::cancelled, "CTP catalog cancelled");
        if (!query)
          query = std::make_unique<ctp::CatalogQuery>(library, flow, std::move(config));
        auto catalog = query->poll(stop.get_token());
        if (!catalog)
          return;
        State ready;
        ready.set_phase("ready");
        ready.set_trading_day(catalog->trading_day);
        for (const auto& entry : catalog->contracts) {
          auto* item = ready.add_contracts();
          item->mutable_instrument()->set_venue(entry.instrument.venue);
          item->mutable_instrument()->set_symbol(entry.instrument.symbol);
          item->set_product(entry.product);
          item->set_name(entry.name);
          item->set_expiry(entry.expiry);
          item->set_contract_id(entry.contract_id);
          item->set_multiplier(entry.multiplier);
          item->set_price_tick(entry.price_tick.str());
        }
        // Keep the live result if an optional offline cache cannot be written.
        if (!stop.stop_requested() && !file.empty()) {
          try {
            replace_file_durably(file, ready.SerializeAsString());
          } catch (const std::exception&) {
            log_process_event("market-data", LogLevel::warning, "catalog.cache_failed", {});
          }
        }
        result = std::move(ready);
      } catch (const Error& error) {
        result.emplace();
        result->set_phase("error");
        result->set_error_code(std::string(error_name(error.code())));
        result->set_diagnostic(error.what());
      } catch (const std::exception&) {
        result.emplace();
        result->set_phase("error");
        result->set_error_code("operation_failed");
        result->set_diagnostic("CTP catalog query failed");
      }
      if (result)
        query.reset(); // API/SPI and library die on the SDK executor.
    }
  };

public:
  explicit CatalogJob(ThreadPool& sdk_owner) : sdk_owner_(sdk_owner) {}
  ~CatalogJob() {
    if (operation_)
      operation_->stop.request_stop();
    if (pending_.valid())
      pending_.get();
    sdk_owner_.submit([this](std::stop_token) { operation_.reset(); }).get();
  }
  void persist_to(std::filesystem::path file) {
    file_ = std::move(file);
    operation_ = std::make_unique<Operation>();
    operation_->file = file_;
    operation_->load_cache = true;
    schedule();
  }
  void start(const std::filesystem::path& library, const std::filesystem::path& flow,
             ctp::CatalogConfiguration config) {
    poll();
    if (operation_)
      throw Error(ErrorCode::conflict, "CTP catalog query is already running");
    operation_ = std::make_unique<Operation>();
    operation_->file = file_;
    operation_->library = library;
    operation_->flow = flow;
    operation_->config = std::move(config);
    cancelled_ = false;
    schedule();
    State loading;
    loading.set_phase("loading");
    publish(std::move(loading));
  }
  void poll() {
    if (!operation_)
      return;
    if (pending_.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready)
      return;
    pending_.get();
    if (!operation_->result) {
      schedule();
      return;
    }
    auto result = std::move(*operation_->result);
    const bool apply = operation_->load_cache || !cancelled_;
    operation_.reset(); // Its SDK resource was released before completing the step.
    if (!apply)
      return;
    if (result.phase() == "ready" || result.phase() == "cached") {
      cached_ = result;
      cached_.set_phase("cached");
    }
    publish(std::move(result));
  }
  void cancel() {
    if (operation_)
      operation_->stop.request_stop();
    cancelled_ = true;
    auto next = cached_;
    if (!next.contracts_size())
      next.set_phase("unconfigured");
    publish(std::move(next));
  }
  bool initializing() const { return operation_ && operation_->load_cache; }
  bool running() const { return bool(operation_); }
  std::pair<std::uint64_t, const State&> snapshot() const { return {revision_, state_}; }

private:
  void schedule() {
    pending_ = sdk_owner_.submit(
        [operation = operation_.get()](std::stop_token) { operation->advance(); });
  }
  void publish(State value) {
    state_ = std::move(value);
    ++revision_;
  }
  ThreadPool& sdk_owner_;
  State state_ = [] {
    State state;
    state.set_phase("unconfigured");
    return state;
  }();
  State cached_;
  std::filesystem::path file_;
  std::uint64_t revision_ = 0;
  std::unique_ptr<Operation> operation_;
  std::future<void> pending_;
  bool cancelled_ = false;
};
} // namespace asterion::market_data
