#include "notice_store.hpp"
#include "contract_notice.hpp"
#include <asterion/protocol/intelligence.hpp>
#include <asterion/protocol/material.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/kernel/process/artifact.hpp>

namespace asterion::intelligence {

PreparedNotice prepare_notice(
    const resource::proto::Owner& owner,
    const proto::NoticeCandidate& candidate,
    const data::proto::MaterialRead& material,
    const std::optional<data::proto::TableListedContracts>& dictionary,
    const programs::Files::Entry& programme)
{
    protocol::validate_notice_candidate(candidate);
    protocol::validate_material_receipt(material.receipt());
    if (material.receipt().reference().SerializeAsString()
        != candidate.material().SerializeAsString())
        throw Error(ErrorCode::conflict, "notice preparation received another material version");
    PreparedNotice result;
    auto& input = result.input_;
    *input.mutable_owner() = owner;
    *input.mutable_candidate() = candidate;
    *input.mutable_material() = material.receipt();
    input.set_program_artifact(programme.digest);
    std::vector<std::string> contracts;
    if (dictionary) {
        protocol::validate_message(*dictionary);
        if (dictionary->query().SerializeAsString() != candidate.dictionary().SerializeAsString())
            throw Error(
                ErrorCode::conflict,
                "notice dictionary response differs from the fixed query");
        auto& ref = *input.mutable_dictionary_reference();
        *ref.mutable_owner() = candidate.material().owner();
        ref.set_kind("table_snapshot");
        ref.set_id(candidate.dictionary().snapshot());
        ref.set_version(ref.id());
        unsigned matches = 0;
        for (const auto& row : dictionary->items()) {
            contracts.push_back(row.contract_id());
            if (row.contract_id() == candidate.contract()) {
                *input.mutable_dictionary_entry() = row;
                ++matches;
            }
        }
        if (matches > 1)
            throw Error(ErrorCode::conflict, "notice dictionary repeats the selected contract");
    }
    protocol::validate_notice_input(input);
    ContractNoticeEvidence evidence;
    const bool readable = material.text().state() == data::proto::MaterialText::READABLE;
    evidence.original = readable ? std::string_view(material.original()) : std::string_view{};
    evidence.contract = candidate.contract();
    if (dictionary)
        evidence.dictionary = std::span<const std::string>(contracts);
    if (input.has_dictionary_entry()) {
        evidence.list_date = input.dictionary_entry().list_date();
        evidence.delist_date = input.dictionary_entry().delist_date();
    }
    const auto span = [](const proto::EvidenceSpan& v) {
        return EvidenceSpan{v.begin(), v.end(), v.text()};
    };
    evidence.subject = span(candidate.subject());
    evidence.parameter = span(candidate.parameter());
    evidence.effective_day = span(candidate.effective_day());
    evidence.percentage = span(candidate.percentage());
    evidence.modality = span(candidate.modality());
    evidence.rule_id = candidate.rule_id();
    evidence.use_scope = candidate.use_scope();
    evidence.source_use_scope = material.receipt().evidence().request().use_scope();
    evidence.use = candidate.use_mode() == proto::POST_HOC_RESEARCH ? NoticeUse::post_hoc_research
        : candidate.use_mode() == proto::STRICT_HISTORY             ? NoticeUse::strict_history
                                                                    : NoticeUse::unspecified;
    evidence.acquired_at_ms = material.receipt().evidence().acquired_at_ms();
    if (material.receipt().evidence().has_source_published_at_ms())
        evidence.source_published_at_ms = material.receipt().evidence().source_published_at_ms();
    const auto assessed = assess_contract_notice(evidence);
    auto& assessment = result.assessment_;
    for (const auto& check : assessed.checks) {
        auto& out = *assessment.add_checks();
        out.set_field(check.field);
        out.set_reason(check.reason);
        out.set_state(
            check.state == CheckState::pass          ? proto::PASS
                : check.state == CheckState::blocked ? proto::BLOCKED
                                                     : proto::UNKNOWN);
    }
    auto& read_check = *assessment.add_checks();
    read_check.set_field("material_readability");
    read_check.set_state(readable ? proto::PASS : proto::BLOCKED);
    read_check.set_reason(
        readable ? "notice_material_readable" : "notice_material_reading_unavailable");
    for (const auto& reason : assessed.limitations)
        assessment.add_limitations(reason);
    if (assessed.margin_ratio)
        assessment.mutable_margin_ratio()->set_units(assessed.margin_ratio->raw());
    assessment.set_effective_day(assessed.effective_day);
    assessment.set_planned_marker_present(assessed.planned_marker_present);
    assessment.set_ready_for_human_review(readable && assessed.ready_for_human_review);
    protocol::validate_notice_assessment(assessment);
    auto& ref = result.reference_;
    *ref.mutable_owner() = owner;
    ref.set_kind("intelligence_candidate");
    ref.set_id(candidate.id());
    ref.set_version(sha256_bytes(input.SerializeAsString()));
    return result;
}

PreparedNoticeChange prepare_notice_change(
    const resource::proto::Owner& owner,
    const proto::NoticeChangeRequest& request,
    const data::proto::MaterialRead& material,
    const programs::Files::Entry& programme)
{
    protocol::validate_notice_change_request(request);
    protocol::validate_material_receipt(material.receipt());
    if (material.receipt().reference().SerializeAsString()
        != request.material().SerializeAsString())
        throw Error(ErrorCode::conflict, "notice change received another material version");
    const auto text = protocol::read_material_text(material.receipt(), material.original());
    const auto& quote = request.evidence();
    if (text.state() != data::proto::MaterialText::READABLE
        || !matches_notice_evidence(
            material.original(),
            {quote.begin(), quote.end(), quote.text()}))
        throw Error(
            ErrorCode::conflict,
            "notice change evidence differs from exact readable original");
    PreparedNoticeChange prepared;
    auto& input = prepared.input_;
    *input.mutable_owner() = owner;
    *input.mutable_request() = request;
    *input.mutable_material() = material.receipt();
    input.set_program_artifact(programme.digest);
    protocol::validate_notice_change_input(input);
    return prepared;
}

} // namespace asterion::intelligence
