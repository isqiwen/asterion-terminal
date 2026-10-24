#include "contract_notice.hpp"
#include <asterion/domain/trading_days.hpp>
#include <asterion/foundation/error.hpp>
#include <algorithm>

namespace asterion::intelligence {
namespace {

bool continuation(unsigned char value)
{
    return (value & 0xc0) == 0x80;
}

} // namespace

bool matches_notice_evidence(std::string_view bytes, const EvidenceSpan& evidence)
{
    return !evidence.text.empty() && evidence.begin < evidence.end && evidence.end <= bytes.size()
        && !continuation(static_cast<unsigned char>(bytes[evidence.begin]))
        && (evidence.end == bytes.size()
            || !continuation(static_cast<unsigned char>(bytes[evidence.end])))
        && bytes.substr(evidence.begin, evidence.end - evidence.begin) == evidence.text;
}

NoticeAssessment assess_contract_notice(const ContractNoticeEvidence& input, std::stop_token stop)
{
    NoticeAssessment result;
    const auto cancelled = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "notice verification cancelled");
    };
    const auto check = [&](const std::string& field, CheckState state, const std::string& reason) {
        cancelled();
        result.checks.push_back({field, state, reason});
    };
    const auto quote = [&](const std::string& field,
                           const EvidenceSpan& evidence) -> std::optional<std::string_view> {
        if (evidence.text.empty()) {
            check(field, CheckState::unknown, "notice_evidence_missing");
            return {};
        }
        const auto bytes = input.original;
        if (!matches_notice_evidence(bytes, evidence)) {
            check(field, CheckState::blocked, "notice_evidence_differs_from_original");
            return {};
        }
        check(field, CheckState::pass, "notice_exact_original_range");
        return bytes.substr(evidence.begin, evidence.end - evidence.begin);
    };
    cancelled();
    check(
        "rule",
        input.rule_id.empty()                                 ? CheckState::unknown
            : input.rule_id == "contract_margin_announcement" ? CheckState::pass
                                                              : CheckState::blocked,
        input.rule_id.empty()                                 ? "notice_rule_not_selected"
            : input.rule_id == "contract_margin_announcement" ? "notice_manual_rule_selected"
                                                              : "notice_rule_unsupported");
    check(
        "use_scope",
        input.use_scope.empty() || input.source_use_scope.empty() ? CheckState::unknown
            : input.use_scope == input.source_use_scope           ? CheckState::pass
                                                                  : CheckState::blocked,
        input.use_scope.empty() || input.source_use_scope.empty() ? "notice_use_scope_missing"
            : input.use_scope == input.source_use_scope ? "notice_source_use_scope_preserved"
                                                        : "notice_source_use_scope_differs");
    const auto subject = quote("subject_evidence", input.subject);
    const auto parameter = quote("parameter_evidence", input.parameter);
    const auto day = quote("effective_day_evidence", input.effective_day);
    const auto percentage = quote("percentage_evidence", input.percentage);
    const auto modality = quote("modality_evidence", input.modality);
    if (subject) {
        if (input.contract.empty() || !input.dictionary)
            check("subject_mapping", CheckState::unknown, "notice_dictionary_mapping_missing");
        else {
            unsigned matches = 0;
            for (const auto& contract : *input.dictionary) {
                cancelled();
                if (contract == input.contract)
                    ++matches;
            }
            check(
                "subject_mapping",
                *subject == input.contract && matches == 1 ? CheckState::pass : CheckState::blocked,
                *subject != input.contract ? "notice_subject_is_not_exact_contract"
                    : matches > 1          ? "notice_contract_mapping_ambiguous"
                    : matches == 0         ? "notice_contract_absent_from_dictionary"
                                           : "notice_exact_contract_mapping");
        }
    }
    if (parameter)
        check(
            "parameter",
            *parameter == "保证金率" || *parameter == "margin rate" ? CheckState::pass
                                                                    : CheckState::blocked,
            *parameter == "保证金率" || *parameter == "margin rate"
                ? "notice_margin_parameter_present"
                : "notice_parameter_unsupported");
    if (day) {
        try {
            (void)parse_trading_date(*day);
            result.effective_day = *day;
            check("effective_day", CheckState::pass, "notice_explicit_effective_day");
        } catch (const std::invalid_argument&) {
            check("effective_day", CheckState::blocked, "notice_effective_day_not_literal");
        }
    }
    if (percentage) {
        bool valid = false;
        try {
            if (percentage->size() > 1 && percentage->back() == '%') {
                const auto value = Decimal::parse(percentage->substr(0, percentage->size() - 1));
                if (value > Decimal{} && value <= Decimal::parse("100")) {
                    result.margin_ratio = divide(value, Decimal::parse("100"));
                    valid = true;
                }
            }
        } catch (const std::invalid_argument&) {
        } catch (const std::overflow_error&) {
        } catch (const std::domain_error&) {
        }
        check(
            "percentage",
            valid ? CheckState::pass : CheckState::blocked,
            valid ? "notice_exact_percentage_conversion" : "notice_percentage_not_supported");
    }
    if (modality) {
        result.planned_marker_present = *modality == "计划" || *modality == "拟"
            || *modality == "planned" || *modality == "proposed";
        check(
            "modality",
            result.planned_marker_present ? CheckState::pass : CheckState::blocked,
            result.planned_marker_present ? "notice_plan_context_requires_human_review"
                                          : "notice_only_planned_announcement_supported");
    }
    if (result.effective_day.empty())
        check("contract_lifetime", CheckState::unknown, "notice_effective_day_unresolved");
    else if (input.list_date.empty() || input.delist_date.empty())
        check("contract_lifetime", CheckState::unknown, "notice_contract_lifetime_missing");
    else {
        try {
            (void)parse_trading_date(input.list_date);
            (void)parse_trading_date(input.delist_date);
            const bool valid = input.list_date <= result.effective_day
                && result.effective_day <= input.delist_date
                && input.list_date <= input.delist_date;
            check(
                "contract_lifetime",
                valid ? CheckState::pass : CheckState::blocked,
                valid ? "notice_effective_day_within_contract_lifetime"
                      : "notice_effective_day_outside_contract_lifetime");
        } catch (const std::invalid_argument&) {
            check("contract_lifetime", CheckState::blocked, "notice_contract_lifetime_invalid");
        }
    }
    const bool publication_known = input.source_published_at_ms && *input.source_published_at_ms > 0
        && *input.source_published_at_ms <= input.acquired_at_ms;
    if (!publication_known)
        result.limitations.push_back(
            input.source_published_at_ms ? "notice_source_publication_time_inconsistent"
                                         : "notice_source_publication_time_unknown");
    if (input.use == NoticeUse::unspecified || input.acquired_at_ms <= 0)
        check("availability_mode", CheckState::unknown, "notice_use_or_acquisition_time_missing");
    else if (input.use == NoticeUse::strict_history && !publication_known)
        check(
            "availability_mode",
            CheckState::unknown,
            "notice_strict_history_requires_source_time");
    else
        check(
            "availability_mode",
            CheckState::pass,
            input.use == NoticeUse::post_hoc_research
                ? "notice_post_hoc_use_only"
                : "notice_source_time_recorded_not_publication_approval");
    result.limitations.push_back("notice_semantic_context_requires_human_review");
    result.ready_for_human_review = std::ranges::all_of(result.checks, [](const auto& c) {
        return c.state == CheckState::pass;
    });
    return result;
}

} // namespace asterion::intelligence
