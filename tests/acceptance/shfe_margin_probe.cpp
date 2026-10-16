// Explicit public-network acceptance probe; never registered as an automatic test.
#include "history_providers.hpp"
#include "historical_margins.hpp"
#include "margins.hpp"
#include <asterion/kernel/native_plugin.hpp>
#include <asterion/protocol/data.hpp>
#include <CLI/CLI.hpp>
#include <iostream>

int main(int argc, char** argv)
{
    CLI::App app{"Verify the installed SHFE provider against public historical reports"};
    std::string directory, plugins;
    app.add_option("--directory", directory)->required();
    app.add_option("--plugins", plugins)->required();
    CLI11_PARSE(app, argc, argv);
    try {
        using namespace asterion;
        if (std::filesystem::exists(directory))
            throw std::invalid_argument("probe requires a new evidence directory");
        configure_native_plugins(plugins);
        unsigned requests = 0;
        auto reader = history_providers::tables(shfe::provider, "", [&](std::stop_token) {
            if (++requests > 6)
                throw std::logic_error("unexpected provider request");
        });
        history_files::TableStore store(directory);
        data::v1::HistoricalMarginQuery query;
        query.set_contract(shfe::contract);
        for (const auto* day : shfe::days) {
            const auto& schema = history_table(shfe::table);
            const auto partition =
                table_partition(schema, "SHFE", day, reader->read(schema, "SHFE", day, {}));
            const auto raw = reader->response();
            if (!raw)
                throw std::logic_error("provider omitted raw evidence");
            const auto saved = store.record(
                partition,
                {shfe::table, 1, shfe::source},
                store.write(partition),
                history_providers::table_artifact(shfe::provider),
                raw->received_at_ns,
                raw);
            query.set_snapshot(saved.revision.snapshot);
            query.add_trading_days(day);
        }
        std::cout
            << protocol::decode_historical_margins(data::historical_margins(store, query)).dump(2)
            << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
