#pragma once
#include <utility>
#include <asterion/research_model.pb.h>
#include <asterion/factor.pb.h>
#include <asterion/foundation/serialization.hpp>

namespace asterion::protocol {

research::proto::ModelReplaySelection encode_model_replay_selection(const Json&);
void validate_model_replay_definition(const research::proto::ModelReplayDefinition&);
Json decode_model_replay_definition(const research::proto::ModelReplayDefinition&);
void validate_model_replay_result(
    const research::proto::ModelReplayDefinition&,
    const research::proto::ModelReplayResult&,
    const data::proto::BarDataset&);
Json decode_model_replay_result(const research::proto::ModelReplayResult&);
void validate_model_decision_request(const research::proto::ModelDecisionTrialRequest&);
void validate_model_decision_input(const research::proto::ModelDecisionTrialInput&);
void validate_model_decision_result(
    const research::proto::ModelDecisionTrialInput&,
    const research::proto::ModelDecisionTrialResult&);
research::proto::ModelDecisionTrialRequest encode_model_decision_request(const Json&);
Json decode_model_decision_request(const research::proto::ModelDecisionTrialRequest&);
Json decode_model_decision_input(const research::proto::ModelDecisionTrialInput&);
Json decode_model_decision_result(
    const research::proto::ModelDecisionTrialInput&,
    const research::proto::ModelDecisionTrialResult&);
void validate_model_time_specification(const research::proto::ModelTimeSpecification&);
void validate_model_time_plan(const research::proto::ModelTimePlan&);
void validate_model_time_input(const research::proto::ModelTimeInput&);
void validate_model_time_result(
    const research::proto::ModelTimeInput&,
    const research::proto::ModelTimeResult&);
std::string model_time_plan_digest(research::proto::ModelTimePlan);
resource::proto::Object model_time_plan_object(const research::proto::ModelTimePlan&);
research::proto::ModelTimeSpecification encode_model_time_specification(const Json&);
Json decode_model_time_specification(const research::proto::ModelTimeSpecification&);
Json decode_model_time_plan(const research::proto::ModelTimePlan&);
Json decode_model_time_input(const research::proto::ModelTimeInput&);
Json decode_model_time_result(
    const research::proto::ModelTimeInput&,
    const research::proto::ModelTimeResult&);
void validate_model_specification(const research::proto::ModelSpecification&, bool complete = true);
void validate_model_draft(const research::proto::ModelDraft&);
void validate_model_mapping_specification(const research::proto::ModelMappingSpecification&);
void validate_model_mapping(const research::proto::ModelMapping&);
std::uint32_t model_mapping_rows(const research::proto::ModelMapping&);
// The ordinal indexes only original validation rows; gaps and final holdout are absent.
std::pair<std::uint32_t, std::uint32_t>
model_mapping_time_position(const research::proto::ModelMapping&, std::uint32_t ordinal);
std::string model_mapping_digest(research::proto::ModelMapping);
resource::proto::Object model_mapping_object(const research::proto::ModelMapping&);
void validate_model_mutation(const research::proto::ModelMutation&);
void validate_model_mutation_reply(
    const research::proto::ModelMutation&,
    const research::proto::ModelReply&,
    const resource::proto::Owner&);
void validate_model_source(const research::proto::ModelSource&);
void validate_model_source(
    const research::proto::ModelSource&,
    const factor::proto::FactorInput&,
    const factor::proto::FactorResult&);
void validate_model_plan(const research::proto::ModelPlan&);
void validate_model_training_input(const research::proto::ModelTrainingInput&);
void validate_model_training_result(
    const research::proto::ModelTrainingInput&,
    const research::proto::ModelTrainingResult&);
std::uint64_t model_training_work_units(const research::proto::ModelTrainingInput&);
std::string model_draft_digest(research::proto::ModelDraft);
std::string model_plan_digest(research::proto::ModelPlan);
resource::proto::Object model_plan_object(const research::proto::ModelPlan&);
resource::proto::Object model_draft_object(const research::proto::ModelDraft&);

research::proto::ModelSpecification encode_model_specification(const Json&);
Json decode_model_specification(const research::proto::ModelSpecification&);
Json decode_model_draft(const research::proto::ModelDraft&);
Json decode_model_plan(const research::proto::ModelPlan&);
research::proto::ModelMappingSpecification encode_model_mapping_specification(const Json&);
Json decode_model_mapping(const research::proto::ModelMapping&);
Json decode_model_reply(
    const research::proto::ModelReply&,
    const research::proto::ModelRequest&,
    const resource::proto::Owner&);
Json decode_model_training_input(const research::proto::ModelTrainingInput&);
Json decode_model_training_result(
    const research::proto::ModelTrainingInput&,
    const research::proto::ModelTrainingResult&);

} // namespace asterion::protocol
