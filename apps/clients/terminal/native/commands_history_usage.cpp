#include "application_impl.hpp"

namespace asterion::terminal {

// Data owns the complete retention decision for this version. Discovery and
// process availability cannot replace or erase that durable reference authority.
PolledTask<Application::Impl::Response> Application::Impl::history_usage(const json& params)
{
    const auto warehouse = data_client;
    if (!warehouse)
        throw Error(ErrorCode::unavailable, "connect the data service first");
    const auto selection_generation = dataset_selection_generation;
    const auto id = text(params, "id");
    bool selected = false;
    for (const auto& selection : selections) {
        const auto& ids = selection.request.source_dataset_ids();
        selected |= std::ranges::find(ids, id) != ids.end();
    }
    auto owned = co_await PollFuture{warehouse->minute_retirement(id, false)};
    if (warehouse != data_client)
        throw Error(ErrorCode::conflict, "data service changed during archive query");
    if (selection_generation != dataset_selection_generation)
        throw Error(ErrorCode::conflict, "dataset selection changed during resolution");
    json report{
        {"dataset_id", id},
        {"references", std::move(owned["references"])},
        {"consumers", std::move(owned["consumers"])},
        {"state", owned.at("state")},
        {"selected_roles", selected ? json::array({"market"}) : json::array()}
    };
    auto result = snapshot();
    result.values["history_usage"] = std::move(report);
    co_return result;
}

} // namespace asterion::terminal
