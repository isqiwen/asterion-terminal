#pragma once
#include "ctp_catalog.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <filesystem>
#include <fstream>
#include <asterion/v1/market.pb.h>
#include <atomic>
#include <mutex>
#include <thread>
namespace asterion::market_data {
// Owns one bounded provider operation. Reading state never waits for SDK I/O.
class CatalogJob {
public:
  ~CatalogJob() { worker_.request_stop(); }
  // The last ready catalog is kept in `file` and offered as phase "cached"
  // (with its trading day) whenever no live query result is available.
  void persist_to(std::filesystem::path file) {
    std::lock_guard lock(mutex_);
    file_ = std::move(file);
    if (std::filesystem::is_regular_file(file_) && !std::filesystem::is_symlink(file_) &&
        std::filesystem::file_size(file_) <= 64 * 1024 * 1024) {
      std::ifstream input(file_, std::ios::binary);
      market::v1::CatalogState stored;
      if (stored.ParseFromIstream(&input) && stored.phase() == "ready" &&
          stored.contracts_size() > 0) {
        stored.set_phase("cached");
        cached_ = std::move(stored);
        state_ = cached_;
        ++revision_;
      }
    }
  }
  void start(const std::filesystem::path& library, const std::filesystem::path& flow,
             ctp::CatalogConfiguration config) {
    if (!done_.load())
      throw Error(ErrorCode::conflict, "CTP catalog query is already running");
    if (worker_.joinable())
      worker_.join();
    std::lock_guard lock(mutex_);
    state_.Clear();
    state_.set_phase("loading");
    ++revision_;
    done_ = false;
    try {
      worker_ = std::jthread(
          [this, library, flow, config = std::move(config)](std::stop_token stop) mutable {
            market::v1::CatalogState result;
            try {
              const auto catalog = ctp::read_catalog(library, flow, std::move(config), stop);
              result.set_phase("ready");
              result.set_trading_day(catalog.trading_day);
              for (const auto& entry : catalog.contracts) {
                auto* item = result.add_contracts();
                item->mutable_instrument()->set_venue(entry.instrument.venue);
                item->mutable_instrument()->set_symbol(entry.instrument.symbol);
                item->set_product(entry.product);
                item->set_name(entry.name);
                item->set_expiry(entry.expiry);
                item->set_contract_id(entry.contract_id);
                item->set_multiplier(entry.multiplier);
                item->set_price_tick(entry.price_tick.str());
              }
            } catch (const Error& error) {
              result.set_phase("error");
              result.set_error_code(std::string(error_name(error.code())));
              result.set_diagnostic(error.what());
            } catch (const std::exception&) {
              result.set_phase("error");
              result.set_error_code("operation_failed");
              result.set_diagnostic("CTP catalog query failed");
            }
            {
              std::lock_guard lock(mutex_);
              if (!stop.stop_requested()) {
                if (result.phase() == "ready" && !file_.empty()) {
                  try {
                    replace_file_durably(file_, result.SerializeAsString());
                    cached_ = result;
                    cached_.set_phase("cached");
                  } catch (const std::exception&) {
                    // The live catalog stays usable; only the offline copy is stale.
                  }
                }
                state_ = std::move(result);
                ++revision_;
              }
            }
            done_ = true;
          });
    } catch (...) {
      done_ = true;
      state_.set_phase("error");
      state_.set_error_code("operation_failed");
      state_.set_diagnostic("CTP catalog worker could not start");
      ++revision_;
      throw;
    }
  }
  void cancel() {
    worker_.request_stop();
    std::lock_guard lock(mutex_);
    if (cached_.contracts_size())
      state_ = cached_;
    else {
      state_.Clear();
      state_.set_phase("unconfigured");
    }
    ++revision_;
  }
  bool running() const { return !done_.load(); }
  std::pair<std::uint64_t, market::v1::CatalogState> snapshot() const {
    std::lock_guard lock(mutex_);
    return {revision_, state_};
  }

private:
  mutable std::mutex mutex_;
  market::v1::CatalogState state_ = [] {
    market::v1::CatalogState s;
    s.set_phase("unconfigured");
    return s;
  }();
  market::v1::CatalogState cached_;
  std::filesystem::path file_;
  std::uint64_t revision_ = 0;
  std::atomic<bool> done_ = true;
  // Declared last: join before destroying callback state.
  std::jthread worker_;
};
} // namespace asterion::market_data
