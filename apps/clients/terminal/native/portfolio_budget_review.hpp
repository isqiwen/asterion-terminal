#pragma once
#include "portfolio_budget.hpp"

namespace asterion::terminal {

inline Json portfolio_budget_review(const Json& source, const Json& limits)
{
    if (source.at("kind") != "backtest")
        throw std::invalid_argument("portfolio budget review requires a backtest result");
    require_fields(limits, {"gross_notional", "margin"});
    Json checks = Json::object(), normalized = Json::object();
    bool supplied = false;
    for (const auto* key : {"gross_notional", "margin"}) {
        std::optional<Decimal> limit;
        if (!limits.at(key).is_null()) {
            limit = Decimal::parse(limits.at(key).get<std::string>());
            supplied = true;
        }
        const auto used = Decimal::parse(source.at("portfolio")
                .at(std::string(key) == "margin" ? "model_margin" : key)
                .get<std::string>());
        const auto check = review_budget(used, limit);
        normalized[key] = limit ? Json(limit->str()) : Json(nullptr);
        checks[key] = {
            {"used", check.used.str()},
            {"limit", normalized[key]},
            {"headroom", check.headroom ? Json(check.headroom->str()) : Json(nullptr)},
            {"state",
                !limit                            ? "unset"
                    : *check.headroom < Decimal{} ? "exceeded"
                                                  : "within"}
        };
    }
    if (!supplied)
        throw std::invalid_argument(
            "portfolio budget review requires at least one nonnegative explicit limit");
    Json points = Json::array(), summary = Json::object();
    for (const auto* key : {"gross_notional", "margin"})
        summary[key] = {
            {"checked_points", 0},
            {"unknown_points", 0},
            {"exceeded_points", normalized.at(key).is_null() ? Json(nullptr) : Json(0)}
        };
    for (const auto& point : source.at("portfolio").at("settlement_path").at("points")) {
        Json row = point;
        row["checks"] = Json::object();
        for (const auto* key : {"gross_notional", "margin"}) {
            const auto& used = point.at(std::string(key) == "margin" ? "model_margin" : key);
            const auto& cap = normalized.at(key);
            if (used.is_null()) {
                row["checks"][key] = {
                    {"used", nullptr},
                    {"limit", cap},
                    {"headroom", nullptr},
                    {"state", cap.is_null() ? "unset" : "unknown"}
                };
                summary[key]["unknown_points"] = summary[key]["unknown_points"].get<unsigned>() + 1;
            } else {
                const auto check = review_budget(
                    Decimal::parse(used.get<std::string>()),
                    cap.is_null() ? std::nullopt
                                  : std::optional{Decimal::parse(cap.get<std::string>())});
                row["checks"][key] = {
                    {"used", used},
                    {"limit", cap},
                    {"headroom", check.headroom ? Json(check.headroom->str()) : Json(nullptr)},
                    {"state",
                        !check.headroom                   ? "unset"
                            : *check.headroom < Decimal{} ? "exceeded"
                                                          : "within"}
                };
                if (check.headroom) {
                    summary[key]["checked_points"] =
                        summary[key]["checked_points"].get<unsigned>() + 1;
                    if (*check.headroom < Decimal{})
                        summary[key]["exceeded_points"] =
                            summary[key]["exceeded_points"].get<unsigned>() + 1;
                }
            }
        }
        points.push_back(std::move(row));
    }
    return {
        {"scope", "saved_account_and_settlement_points_review_only"},
        {"settlement_checks", std::move(points)},
        {"settlement_summary", std::move(summary)},
        {"source",
            {{"task_id", source.at("id")},
                {"input_sha256", source.at("input_sha256")},
                {"result_digest", source.at("task").at("result_digest")}}},
        {"limits", std::move(normalized)},
        {"checks", std::move(checks)},
        {"portfolio", source.at("portfolio")}
    };
}

} // namespace asterion::terminal
