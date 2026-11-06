#include "catalog_source.hpp"
#include <asterion/quotes.pb.h>
#include <algorithm>
#include <fstream>

namespace asterion::history::acquisition {

CatalogSource::CatalogSource(std::filesystem::path snapshots, std::stop_token stop)
    : snapshots_(std::move(snapshots)),
      stop_(stop)
{}

bool CatalogSource::supports(std::string_view table, std::string_view exchange) const
{
    require_exchange(exchange);
    return table == "price_ticks";
}

Rows CatalogSource::price_ticks(const std::string& exchange, const std::string& day)
{
    require_exchange(exchange);
    const auto date = day_of(day);
    check_stop(stop_);
    auto compact = day;
    std::erase(compact, '-');
    const auto file = snapshots_ / (compact + ".pb");
    if (!std::filesystem::exists(file))
        return {};
    ++requests_;
    std::ifstream input(file, std::ios::binary);
    ::quotes::Catalog catalog;
    if (!catalog.ParseFromIstream(&input))
        throw SourceError("invalid_data", "damaged contract snapshot: " + file.string());
    Rows rows;
    for (const auto& item : catalog.contracts()) {
        // A delivery month CTP did not state names no contract.
        if (!item.contract().starts_with(exchange + "/"))
            continue;
        const auto first = item.contract().find('/'), last = item.contract().rfind('/');
        rows.push_back(
            {exchange,
                item.contract().substr(first + 1, last - first - 1),
                item.contract(),
                day,
                day_text(date + std::chrono::days(1)),
                item.price_tick().text(),
                std::string("ctp")});
    }
    return rows;
}

} // namespace asterion::history::acquisition
