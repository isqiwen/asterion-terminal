#pragma once
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <nlohmann/json.hpp>
#include <asterion/base/serialization.hpp>
#include <set>
#include <filesystem>
#include <vector>

namespace asterion::terminal {

// A traceable result document, deliberately not a self-contained replay bundle.
inline std::string
research_evidence_document(const nlohmann::json& source, const std::string& instance)
{
    const auto kind = source.at("kind").get<std::string>();
    if (kind != "backtest" && kind != "factor")
        throw std::invalid_argument(
            "research evidence export requires a backtest or factor result");
    const auto& task = source.at("task");
    const auto programme = task.value("program_artifact", std::string{});
    if (programme.size() != 64
        || programme.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("research evidence export requires a fixed execution program");
    nlohmann::json payload = {
        {"format", "asterion.research.evidence"},
        {"scope", "verified_parameter_summary_and_complete_result"},
        {"source",
            {{"task_instance", instance},
                {"task_id", source.at("id")},
                {"kind", kind},
                {"attempt", task.at("attempt")},
                {"result_sha256", task.at("result_digest")},
                {"program_artifact", programme},
                {"input_sha256",
                    source.contains("input_sha256") ? source.at("input_sha256")
                                                    : nlohmann::json(nullptr)}}},
        {"description", task.at("metadata")},
        {"experiment", source.at("experiment")},
        {"result", source.at("result")},
        {"included",
            {"fixed input parameter and dataset summaries",
                "verified complete calculation result",
                "current name, notes and recorded derivation"}},
        {"not_included",
            {"input.pb",
                "raw market bars and source tables",
                "historical archives",
                "native plugins and executables"}},
        {"limitations",
            {"This document alone cannot reproduce the calculation.",
                "Source SHA-256 values identify service-owned Protobuf bytes, not this JSON.",
                "Descriptions are editable and are not part of calculation identity."}}
    };
    if (task.contains("strategy_use"))
        payload["strategy_use"] = task.at("strategy_use");
    if (kind == "backtest") {
        payload["performance"] = source.at("performance");
        payload["trades"] = source.at("trades");
        payload["reconciliation"] = source.at("reconciliation");
        payload["portfolio"] = source.at("portfolio");
        payload["methodology"] = {
            "Performance uses day-end equity after the configured costs and slippage.",
            "Monthly contributions group recorded trading dates; net P&L divided by the original " "deposit adds to total return. Gross P&L adds back fees only, not slippage. Missing months " "are not filled; the first and last months may be partial.",
            "Overall includes training days; holdout inherits development equity; rolling " "out-of-sample starts at the first validation round.",
            "Null metrics are undefined; absent evaluation segments are not applicable. No ranking or " "parameter reselection is performed."
        };
    } else
        payload["methodology"] = {
            "Single-series statistics are time-series correlations; multiple-series statistics are " "cross-sectional IC.",
            "Factor group spreads are label-return differences, not account returns after trading " "costs.",
            "Original partitions and folds are retained separately; no common-sample recomputation, " "pooling or reselection is performed.",
            "The original input SHA-256 is not exposed for factor results and is null here."
        };
    return payload.dump(2) + "\n";
}

inline std::string comparison_evidence_document(
    const std::vector<nlohmann::json>& sources,
    const std::string& instance,
    const nlohmann::json& matrix)
{
    if (sources.size() < 2 || sources.size() > 3 || matrix.at("columns").size() != sources.size()
        || matrix.at("reference_id") != sources.front().at("id"))
        throw std::invalid_argument("invalid research comparison evidence");
    require_fields(matrix, {"language", "reference_id", "columns", "rows"});
    if (!matrix.at("language").is_string() || !matrix.at("rows").is_array()
        || matrix.at("rows").empty())
        throw std::invalid_argument("invalid research comparison evidence");
    const std::set<std::string> states = {
        "defined",
        "undefined",
        "not_applicable",
        "missing",
        "no_observations",
        "metric_group",
        "display_only"
    };
    for (const auto& column : matrix.at("columns")) {
        require_fields(column, {"id", "methodology", "reasons"});
        if (!column.at("reasons").is_array()
            || (column.at("methodology") == "comparable"
                    ? !column.at("reasons").empty()
                    : column.at("methodology") != "independent" || column.at("reasons").empty()))
            throw std::invalid_argument("invalid research comparison evidence");
        for (const auto& reason : column.at("reasons"))
            if (!reason.is_string())
                throw std::invalid_argument("invalid research comparison evidence");
    }
    for (const auto& row : matrix.at("rows")) {
        require_fields(row, {"label", "cells"});
        if (!row.at("label").is_string() || !row.at("cells").is_array()
            || row.at("cells").size() != sources.size())
            throw std::invalid_argument("invalid research comparison evidence");
        for (const auto& cell : row.at("cells")) {
            require_fields(cell, {"text", "state", "metrics"});
            if (!cell.at("text").is_string()
                || !states.contains(cell.at("state").get<std::string>())
                || !cell.at("metrics").is_array())
                throw std::invalid_argument("invalid research comparison evidence");
            for (const auto& metric : cell.at("metrics")) {
                require_fields(metric, {"label", "text", "state"});
                if (!metric.at("label").is_string() || !metric.at("text").is_string()
                    || !states.contains(metric.at("state").get<std::string>()))
                    throw std::invalid_argument("invalid research comparison evidence");
            }
        }
    }
    nlohmann::json members = nlohmann::json::array();
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].at("kind") != sources.front().at("kind")
            || matrix.at("columns").at(i).at("id") != sources[i].at("id"))
            throw std::invalid_argument("invalid research comparison evidence");
        members.push_back(nlohmann::json::parse(research_evidence_document(sources[i], instance)));
    }
    const nlohmann::json document = {
        {"format", "asterion.research.comparison.evidence"},
        {"scope", "rendered_comparison_summary_with_verified_member_evidence"},
        {"matrix", matrix},
        {"experiments", std::move(members)},
        {"limitations",
            {"The matrix is the Terminal display and comparison-rule summary, not a service calculation " "or independent replay package.",
                "Displayed metric text is rounded; original member results retain their original values.",
                "No ranking, pooling, common-sample recalculation or parameter reselection is performed.",
                "Raw market data, input.pb, source tables, archives and plugins are not included."}},
        {"value_states",
            {"defined",
                "undefined",
                "not_applicable",
                "missing",
                "no_observations",
                "metric_group",
                "display_only"}}
    };
    return document.dump(2) + "\n";
}

inline void
save_research_evidence(const std::filesystem::path& destination, const std::string& content)
{
    namespace fs = std::filesystem;
    if (!destination.is_absolute() || fs::exists(destination) || fs::is_symlink(destination))
        throw std::invalid_argument("choose a new file path for research evidence");
    const auto temporary =
        destination.parent_path() / (".asterion-evidence-" + unique_process_id());
    try {
        write_file_durably(temporary, content);
        // Atomic no-replace publication: an existing destination, including a race, is never
        // overwritten.
        fs::create_hard_link(temporary, destination);
        sync_directory(destination.parent_path());
        fs::remove(temporary);
        sync_directory(destination.parent_path());
    } catch (...) {
        std::error_code ignored;
        fs::remove(temporary, ignored);
        throw;
    }
}

} // namespace asterion::terminal
