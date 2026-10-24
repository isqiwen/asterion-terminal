#pragma once
#include <asterion/foundation/decimal.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace asterion::intelligence {

struct EvidenceSpan
{
    std::uint64_t begin = 0, end = 0;
    std::string text;
};
enum class CheckState
{
    pass,
    blocked,
    unknown
};

struct NoticeCheck
{
    std::string field;
    CheckState state;
    std::string reason;
};
enum class NoticeUse
{
    unspecified,
    post_hoc_research,
    strict_history
};

struct ContractNoticeEvidence
{
    // Caller already verified the immutable Data receipt/body and UTF-8 coverage.
    // The rule does not re-fetch, re-hash or normalize the original.
    std::string_view original;
    std::string contract;
    std::string list_date, delist_date;
    std::optional<std::span<const std::string>> dictionary;
    EvidenceSpan subject, parameter, effective_day, percentage, modality;
    std::string rule_id, use_scope, source_use_scope;
    NoticeUse use = NoticeUse::unspecified;
    std::int64_t acquired_at_ms = 0;
    std::optional<std::int64_t> source_published_at_ms;
};

struct NoticeAssessment
{
    std::vector<NoticeCheck> checks;
    std::vector<std::string> limitations;
    std::optional<Decimal> margin_ratio;
    std::string effective_day;
    bool planned_marker_present = false;
    // Not semantic approval, human verification or publication authorization.
    bool ready_for_human_review = false;
};

bool matches_notice_evidence(std::string_view original, const EvidenceSpan&);

NoticeAssessment assess_contract_notice(const ContractNoticeEvidence&, std::stop_token = {});

} // namespace asterion::intelligence
