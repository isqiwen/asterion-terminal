#include "response_evidence.hpp"
#include "wire.hpp"
#include <regex>
#include <asterion/futures/history_identity.hpp>
#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace asterion::tushare {

bool allow_daily_response(
    const std::string& body,
    const std::string& venue,
    const std::string& day) noexcept
{
    try {
        if (venue != "SHFE" || day.size() != 10 || body.empty()
            || body.size() > max_source_response_bytes)
            return false;
        // Reject duplicate keys before inspecting the parsed object: overwritten
        // fields must not hide a credential/message in retained original bytes.
        std::vector<std::set<std::string>> keys;
        auto inspect = [&](int, Json::parse_event_t event, Json& value) {
            if (event == Json::parse_event_t::object_start)
                keys.emplace_back();
            if (event == Json::parse_event_t::key
                && !keys.back().insert(value.get<std::string>()).second)
                throw std::invalid_argument("duplicate response key");
            if (event == Json::parse_event_t::object_end)
                keys.pop_back();
            return true;
        };
        const auto inspected = Json::parse(body, inspect);
        if (!inspected.is_object())
            return false;
        const auto root = parse_response(body);
        const std::set<std::string> envelope{"request_id", "code", "msg", "data"};
        if (!root.is_object())
            return false;
        for (const auto& [key, value] : root.items())
            if (!envelope.contains(key))
                return false;
        if (!root.contains("code") || text(root.at("code")) != "0" || !root.contains("data"))
            return false;
        if (root.contains("msg") && !root.at("msg").is_null() && root.at("msg") != "")
            return false;
        if (root.contains("request_id")
            && !std::regex_match(
                text(root.at("request_id")),
                std::regex(
                    "[0-9a-fA-F]{32}|[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-" "[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")))
            return false;
        const auto& data = root.at("data");
        if (!data.is_object())
            return false;
        for (const auto& [key, value] : data.items())
            if (key != "fields" && key != "items" && key != "has_more")
                return false;
        if (data.contains("has_more") && data.at("has_more") != false)
            return false;
        const std::set<std::string> fields{
            "ts_code",
            "trade_date",
            "pre_close",
            "pre_settle",
            "open",
            "high",
            "low",
            "close",
            "settle",
            "vol",
            "amount",
            "oi",
            "oi_chg",
            "delv_settle"
        };
        const auto& names = data.at("fields");
        const auto& rows = data.at("items");
        if (!names.is_array() || names.size() != fields.size() || !rows.is_array() || rows.empty()
            || rows.size() >= 2000)
            return false;
        std::set<std::string> found;
        for (const auto& field : names)
            if (!field.is_string() || !fields.contains(field.get<std::string>())
                || !found.insert(field.get<std::string>()).second)
                return false;
        auto compact = day;
        compact.erase(7, 1);
        compact.erase(4, 1);
        const std::regex code("[A-Za-z]{1,3}[0-9]{0,4}\\.SHF"),
            number("-?[0-9]{1,18}(\\.[0-9]{1,12})?([eE][+-]?[0-9]{1,2})?");
        for (const auto& row : rows) {
            if (!row.is_array() || row.size() != names.size())
                return false;
            for (std::size_t i = 0; i < names.size(); ++i) {
                const auto name = names[i].get<std::string>();
                const auto& value = row[i];
                if (name == "ts_code") {
                    if (!value.is_string() || !std::regex_match(text(value), code))
                        return false;
                } else if (name == "trade_date") {
                    if (!value.is_string() || text(value) != compact)
                        return false;
                } else if (!value.is_null()
                    && (!value.is_string() || !std::regex_match(text(value), number)))
                    return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

DailyResponseReview review_daily_response(
    const std::optional<SourceResponse>& response,
    const TablePartition& partition)
{
    DailyResponseReview result;
    result.normalized_rows = partition.rows.size();
    if (!partition.table || partition.table->id != "futures.daily"
        || partition.exchange != "SHFE") {
        result.reason = "unsupported_scope";
        return result;
    }
    if (!response)
        return result;
    if (response->format != daily_response_format
        || !allow_daily_response(response->body, partition.exchange, partition.period)) {
        result.reason = "unsupported_response_shape";
        return result; // Never return unallowlisted text in findings.
    }
    const auto root = parse_response(response->body);
    const auto& data = root.at("data");
    std::map<std::string, std::size_t> fields;
    for (std::size_t c = 0; c < data.at("fields").size(); ++c)
        fields.emplace(text(data.at("fields")[c]), c);
    result.response_fields = fields.size();
    result.response_rows = data.at("items").size();
    const auto& table = *partition.table;
    std::map<std::string, const std::vector<TableCell>*> normalized;
    for (const auto& row : partition.rows) {
        const auto id =
            HistoryIdentity::parse(std::get<std::string>(row[table_column(table, "contract")]));
        auto product = id.product;
        std::ranges::transform(product, product.begin(), [](unsigned char c) {
            return std::toupper(c);
        });
        // Derive the vendor key FROM the stored full identity. Never infer the
        // century or listing from a two-digit vendor year.
        const auto code =
            product + id.delivery_month.substr(2, 2) + id.delivery_month.substr(5, 2) + ".SHF";
        if (!normalized.emplace(code, &row).second)
            result.unknown_keys.push_back(code); // repeated century cannot be resolved here
    }
    const std::array<std::pair<const char*, const char*>, 12> mapping{
        {{"open", "open"},
            {"high", "high"},
            {"low", "low"},
            {"close", "close"},
            {"settle", "settlement"},
            {"pre_close", "previous_close"},
            {"pre_settle", "previous_settlement"},
            {"vol", "volume"},
            {"amount", "amount"},
            {"oi", "open_interest"},
            {"oi_chg", "open_interest_change"},
            {"delv_settle", "delivery_settlement"}}
    };
    std::set<std::string> seen;
    bool incomplete_keys = false;
    const std::regex dated("[A-Z]{1,3}[0-9]{4}\\.SHF"), short_dated("[A-Z]{1,3}[0-9]{3}\\.SHF");
    for (const auto& row : data.at("items")) {
        const auto code = text(row[fields.at("ts_code")]);
        if (!seen.insert(code).second) {
            result.differences.push_back({"duplicate_response_key", code, "ts_code", {}, {}, {}});
            continue;
        }
        if (!std::regex_match(code, dated)) {
            if (std::regex_match(code, short_dated)) {
                incomplete_keys = true;
                result.unknown_keys.push_back(code);
            } else
                result.skipped_keys.push_back(code); // same non-dated exclusion as the source
            continue;
        }
        ++result.dated_rows;
        const auto found = normalized.find(code);
        if (found == normalized.end()) {
            result.differences.push_back(
                {"response_key_without_partition", code, "contract", {}, {}, {}});
            continue;
        }
        if (std::ranges::find(result.unknown_keys, code) != result.unknown_keys.end())
            continue;
        ++result.matched_rows;
        for (const auto& [source, target] : mapping) {
            const auto c = table_column(table, target);
            const auto& column = table.columns[c];
            const auto& cell = row[fields.at(source)];
            const auto actual = table_cell_text(column, (*found->second)[c]);
            std::optional<std::string> raw, expected;
            bool exact = true;
            if (!cell.is_null()) {
                raw = text(cell);
                try {
                    expected = plain_decimal(cell, std::string_view(source) == "amount" ? 4 : 0);
                    if (c >= table_column(table, "open") && c <= table_column(table, "close")
                        && *expected == "0")
                        expected.reset();
                    else
                        expected = table_cell_text(
                            column,
                            table_cell(column, partition.exchange, *expected));
                } catch (const std::invalid_argument&) {
                    exact = false;
                }
            }
            ++result.compared_cells;
            if (!exact)
                result.differences.push_back(
                    {"not_exactly_representable", code, target, raw, expected, actual});
            else if (expected != actual)
                result.differences.push_back(
                    {"value_mismatch", code, target, raw, expected, actual});
        }
    }
    for (const auto& [code, row] : normalized)
        if (!seen.contains(code)) {
            if (incomplete_keys)
                result.unknown_keys.push_back(code); // an unsupported short code might name it
            else
                result.differences.push_back(
                    {"partition_key_without_response", code, "contract", {}, {}, {}});
        }
    result.status = !result.differences.empty() ? "differences"
        : !result.unknown_keys.empty()          ? "unknown"
                                                : "consistent";
    result.reason =
        result.unknown_keys.empty() ? "checked_recorded_partition" : "key_mapping_unverified";
    return result;
}

} // namespace asterion::tushare
