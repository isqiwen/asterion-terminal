#include "csv_market_data.hpp"

#include <charconv>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace asterion {
namespace {
void remove_cr(std::string& line) {
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
}
TradeTick parse_row(const std::string& row, const Instrument& instrument) {
  const auto first = row.find(',');
  const auto second = first == std::string::npos ? std::string::npos : row.find(',', first + 1);
  if (second == std::string::npos || row.find(',', second + 1) != std::string::npos) {
    throw std::invalid_argument("expected timestamp_ns,price,quantity");
  }
  std::string_view timestamp(row.data(), first);
  if (timestamp.empty() || (timestamp.size() > 1 && timestamp.front() == '0')) {
    throw std::invalid_argument("invalid UTC epoch nanoseconds");
  }
  std::int64_t ns = 0;
  const auto [end, error] =
      std::from_chars(timestamp.data(), timestamp.data() + timestamp.size(), ns);
  if (error != std::errc{} || end != timestamp.data() + timestamp.size() || ns < 0) {
    throw std::invalid_argument("invalid UTC epoch nanoseconds");
  }
  TradeTick tick{instrument.id, ns,
                 Decimal::parse(std::string_view(row).substr(first + 1, second - first - 1)),
                 Decimal::parse(std::string_view(row).substr(second + 1))};
  tick.validate(instrument);
  return tick;
}
} // namespace

CsvMarketData::CsvMarketData(std::filesystem::path path, Instrument instrument)
    : path_(std::move(path)), instrument_(std::move(instrument)) {
  instrument_.validate();
  if (path_.empty())
    throw std::invalid_argument("CSV path is required");
}
CsvMarketData::CsvMarketData(Instrument instrument, std::string csv_snapshot)
    : instrument_(std::move(instrument)), snapshot_(std::move(csv_snapshot)) {
  instrument_.validate();
}
PluginDescriptor CsvMarketData::descriptor() const {
  return {"asterion.data.csv", PluginKind::data, plugin_contract_version, {}};
}
void CsvMarketData::start() {
  if (started_)
    throw std::logic_error("CSV source is already started");
  if (snapshot_)
    input_ = std::make_unique<std::istringstream>(*snapshot_);
  else
    input_ = std::make_unique<std::ifstream>(path_, std::ios::binary);
  try {
    if (!*input_)
      throw std::runtime_error("cannot open CSV input");
    std::string header;
    std::getline(*input_, header);
    remove_cr(header);
    if (header != "timestamp_ns,price,quantity") {
      throw std::invalid_argument("unsupported CSV header; expected timestamp_ns,price,quantity");
    }
    previous_timestamp_ = -1;
    line_number_ = 1;
    failed_ = false;
    started_ = true;
  } catch (...) {
    input_.reset();
    throw;
  }
}
void CsvMarketData::stop() noexcept {
  input_.reset();
  started_ = false;
}
std::optional<TradeTick> CsvMarketData::next() {
  if (!started_ || failed_)
    throw std::logic_error("CSV source is stopped or failed");
  try {
    std::string line;
    if (!std::getline(*input_, line)) {
      if (input_->bad() || !input_->eof())
        throw std::runtime_error("CSV read failed");
      return std::nullopt;
    }
    ++line_number_;
    remove_cr(line);
    auto tick = parse_row(line, instrument_);
    if (tick.timestamp_ns < previous_timestamp_)
      throw std::invalid_argument("timestamps must be nondecreasing");
    previous_timestamp_ = tick.timestamp_ns;
    return tick;
  } catch (const std::exception& error) {
    failed_ = true;
    throw std::runtime_error("CSV line " + std::to_string(line_number_) + ": " + error.what());
  }
}
} // namespace asterion
