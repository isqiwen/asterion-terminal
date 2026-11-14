#pragma once
#include "ctp_catalog.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/thread_pool.hpp>
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/market.hpp>
#include <asterion/protocol/trading.hpp>
#include <filesystem>
#include <fstream>

namespace asterion::market {

// The CTP contract catalog: the cached copy at start, then each refresh's
// result. Queries run on the SDK executor, one at a time; the market owner
// polls for their results.
class Catalog
{
    using State = ::market::Catalog;

    struct Operation
    {
        void advance()
        {
            try {
                if (stop.stop_requested())
                    throw Error(ErrorCode::cancelled, "CTP catalog cancelled");
                if (!query)
                    query = std::make_unique<ctp::CatalogQuery>(library, flow, std::move(config));
                auto catalog = query->poll(stop.get_token());
                if (!catalog)
                    return;
                State ready;
                ready.mutable_status()->set_state(::market::CATALOG_STATE_READY);
                ready.mutable_status()->set_trading_day(protocol::iso_day(catalog->trading_day));
                for (const auto& entry : catalog->contracts) {
                    auto* item = ready.add_contracts();
                    *item->mutable_instrument() = protocol::encode_instrument(entry.instrument);
                    item->set_contract(entry.contract_id);
                    item->set_product(entry.product);
                    item->set_name(entry.name);
                    item->set_expiry_day(protocol::iso_day(entry.expiry));
                    item->set_multiplier(entry.multiplier);
                    *item->mutable_price_tick() = protocol::decimal(entry.price_tick);
                }
                // Keep the live result if the optional offline cache cannot be written.
                if (!stop.stop_requested() && !file.empty()) {
                    try {
                        replace_file_durably(file, ready.SerializeAsString());
                    } catch (const std::exception&) {
                        log::warn("the contract catalog cache could not be written");
                    }
                }
                result = std::move(ready);
            } catch (const Error& error) {
                unavailable = error.code() == ErrorCode::unavailable;
                result.emplace();
                result->mutable_status()->set_state(::market::CATALOG_STATE_FAILED);
                result->mutable_status()->set_error(error.what());
            } catch (const std::exception&) {
                result.emplace();
                result->mutable_status()->set_state(::market::CATALOG_STATE_FAILED);
                result->mutable_status()->set_error("CTP catalog query failed");
            }
            if (result)
                query.reset(); // API/SPI and library die on the SDK executor.
        }

        std::filesystem::path file, library, flow;
        ctp::CatalogConfiguration config;
        std::stop_source stop;
        std::unique_ptr<ctp::CatalogQuery> query;
        std::optional<State> result;
        bool unavailable = false;
    };

public:
    // Starts from the copy saved by the last successful query, if there is one;
    // a damaged copy is reported as a failed catalog until the next refresh.
    Catalog(ThreadPool& sdk_owner, std::filesystem::path file)
        : sdk_owner_(sdk_owner),
          file_(std::move(file))
    {
        if (!std::filesystem::exists(file_))
            return;
        State cached;
        std::ifstream input(file_, std::ios::binary);
        if (std::filesystem::is_regular_file(file_)
            && std::filesystem::file_size(file_) <= 64 * 1024 * 1024
            && cached.ParseFromIstream(&input)
            && cached.status().state() == ::market::CATALOG_STATE_READY
            && cached.contracts_size() <= 20000) {
            cached.mutable_status()->set_state(::market::CATALOG_STATE_CACHED);
            cached_ = cached;
            publish(std::move(cached));
        } else {
            State failed;
            failed.mutable_status()->set_state(::market::CATALOG_STATE_FAILED);
            failed.mutable_status()->set_error("invalid cached CTP catalog");
            publish(std::move(failed));
        }
    }

    ~Catalog()
    {
        if (operation_)
            operation_->stop.request_stop();
        if (pending_.valid())
            pending_.get();
        sdk_owner_
            .submit([this](std::stop_token) {
                operation_.reset();
            })
            .get();
    }

    void refresh(
        const std::filesystem::path& library,
        const std::filesystem::path& flow,
        ctp::CatalogConfiguration config)
    {
        operation_ = std::make_unique<Operation>();
        operation_->file = file_;
        operation_->library = library;
        operation_->flow = flow;
        operation_->config = std::move(config);
        schedule();
        State loading = cached_;
        loading.mutable_status()->set_state(::market::CATALOG_STATE_LOADING);
        publish(std::move(loading));
    }

    void poll()
    {
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
        unavailable_ = operation_->unavailable;
        operation_.reset(); // Its SDK resource was released before completing the step.
        const auto state = result.status().state();
        if (state == ::market::CATALOG_STATE_READY || state == ::market::CATALOG_STATE_CACHED) {
            cached_ = result;
            cached_.mutable_status()->set_state(::market::CATALOG_STATE_CACHED);
        } else if (state == ::market::CATALOG_STATE_FAILED) {
            log::warn("contract catalog refresh failed: {}", result.status().error());
            // A failed refresh reports its error while retaining known contracts.
            *result.mutable_contracts() = cached_.contracts();
            result.mutable_status()->set_trading_day(cached_.status().trading_day());
        }
        publish(std::move(result));
    }

    bool running() const
    {
        return bool(operation_);
    }

    std::chrono::seconds retry_delay() const
    {
        return std::chrono::seconds{unavailable_ ? 10 : 300};
    }

    const State& state() const
    {
        return state_;
    }

    // Changes with every published state.
    std::uint64_t revision() const
    {
        return revision_;
    }

private:
    void schedule()
    {
        pending_ = sdk_owner_.submit([operation = operation_.get()](std::stop_token) {
            operation->advance();
        });
    }

    void publish(State value)
    {
        state_ = std::move(value);
        ++revision_;
    }

    ThreadPool& sdk_owner_;
    State state_ = [] {
        State state;
        state.mutable_status()->set_state(::market::CATALOG_STATE_NONE);
        return state;
    }();
    std::filesystem::path file_;
    State cached_;
    std::uint64_t revision_ = 0;
    std::unique_ptr<Operation> operation_;
    std::future<void> pending_;
    bool unavailable_ = false;
};

} // namespace asterion::market
