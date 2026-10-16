#pragma once
// Test-only reduced official report shapes, never a production import path.
#include "../plugins/data/shfe/margins.hpp"
#include "history_tables.hpp"
#include <asterion/foundation/serialization.hpp>
#include <fstream>

namespace asterion::test {

inline SourceResponse shfe_margin_response(const std::string& day)
{
    std::ifstream file(
        std::filesystem::path(__FILE__).parent_path() / "fixtures/shfe_margin_reports.json");
    Json fixture;
    file >> fixture;
    const auto& reports = fixture.at(day);
    return {
        shfe::response_format,
        Json{
            {"trading_day_start", reports.at("trading_day_start").dump()},
            {"settlement", reports.at("settlement").dump()}
        }.dump(),
        1800000000000000000
    };
}

inline std::string keep_shfe_margin(
    history_files::TableStore& store,
    const std::string& day,
    const SourceResponse& raw)
{
    const auto partition =
        table_partition(history_table(shfe::table), "SHFE", day, shfe::rows(raw, day));
    return store
        .record(
            partition,
            {shfe::table, 1, shfe::source},
            store.write(partition),
            std::string(64, 'e'),
            raw.received_at_ns,
            raw)
        .revision.snapshot;
}

inline std::string seed_shfe_margins(const std::filesystem::path& root)
{
    history_files::TableStore store(root / "tables");
    std::string snapshot;
    for (const auto* day : shfe::days)
        snapshot = keep_shfe_margin(store, day, shfe_margin_response(day));
    return snapshot;
}

} // namespace asterion::test
