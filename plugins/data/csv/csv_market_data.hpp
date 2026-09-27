#pragma once

#include <asterion/domain/market_data_port.hpp>

#include <filesystem>
#include <istream>
#include <memory>

namespace asterion {

class CsvMarketData final : public MarketDataPort {
public:
    CsvMarketData(std::filesystem::path path, Instrument instrument);
    CsvMarketData(Instrument instrument, std::string csv_snapshot);
    [[nodiscard]] PluginDescriptor descriptor() const override;
    [[nodiscard]] const Instrument& instrument() const noexcept override { return instrument_; }
    void start() override;
    void stop() noexcept override;
    std::optional<TradeTick> next() override;

private:
    std::filesystem::path path_;
    Instrument instrument_;
    std::unique_ptr<std::istream> input_;
    std::optional<std::string> snapshot_;
    std::int64_t previous_timestamp_ = -1;
    std::size_t line_number_ = 1;
    bool started_ = false;
    bool failed_ = false;
};
} // namespace asterion
