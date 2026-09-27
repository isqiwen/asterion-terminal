#include <asterion/kernel/plugin.hpp>
#include <asterion/kernel/event_bus.hpp>
#include "csv_market_data.hpp"
#include "runtime_info.hpp"

#include <CLI/CLI.hpp>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
asterion::AssetClass asset_class(std::string_view name) {
    using asterion::AssetClass;
    if (name == "equity") return AssetClass::equity;
    if (name == "futures") return AssetClass::futures;
    if (name == "option") return AssetClass::option;
    if (name == "crypto") return AssetClass::crypto;
    if (name == "fx") return AssetClass::fx;
    if (name == "bond") return AssetClass::bond;
    if (name == "commodity") return AssetClass::commodity;
    throw std::invalid_argument("unsupported asset class");
}
}

int main(int argc, char** argv) {
    CLI::App app{"Asterion diagnostics and historical data tools"};
    app.set_version_flag("--version", "Asterion 0.1.0");
    std::string file, venue, symbol, asset, currency, price_step, qty_step, multiplier;
    auto* replay = app.add_subcommand("replay-csv", "Validate historical trades from a CSV file");
    replay->add_option("FILE", file)->required()->check(CLI::ExistingFile);
    replay->add_option("VENUE", venue)->required();
    replay->add_option("SYMBOL", symbol)->required();
    replay->add_option("ASSET", asset)->required()->check(CLI::IsMember({"equity", "futures", "option", "crypto", "fx", "bond", "commodity"}));
    replay->add_option("CURRENCY", currency)->required();
    replay->add_option("PRICE_STEP", price_step)->required();
    replay->add_option("QTY_STEP", qty_step)->required();
    replay->add_option("MULTIPLIER", multiplier)->required();
    argv = app.ensure_utf8(argv);
    CLI11_PARSE(app, argc, argv);
    try {
        asterion::PluginManager plugins;
        plugins.add(asterion::make_runtime_info());
        if (*replay) {
            using asterion::Decimal;
            asterion::Instrument instrument{{venue, symbol}, asset_class(asset), currency,
                Decimal::parse(price_step), Decimal::parse(qty_step), Decimal::parse(multiplier)};
            auto source = std::make_unique<asterion::CsvMarketData>(std::filesystem::path(std::u8string(file.begin(), file.end())), instrument);
            auto* stream = source.get();
            plugins.add(std::move(source));
            plugins.start();
            asterion::EventBus<asterion::TradeTick> events;
            std::size_t count = 0;
            std::optional<asterion::TradeTick> last;
            events.subscribe([&](const auto& tick) { ++count; last = tick; });
            while (auto tick = stream->next()) events.publish(*tick);
            std::cout << "Historical CSV validated | " << instrument.id.venue << '/' << instrument.id.symbol
                      << " | trades=" << count << '\n';
            if (last) std::cout << "Last timestamp_ns=" << last->timestamp_ns << " price=" << last->price.str() << '\n';
            return 0;
        }
        plugins.start();
        for (const auto& descriptor : plugins.descriptors()) {
            std::cout << descriptor.id << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Asterion startup failed: " << error.what() << '\n';
        return 1;
    }
}
