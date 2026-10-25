#pragma once
#include <asterion/research_batch.pb.h>
#include <asterion/research_assessment.pb.h>
#include <asterion/task.pb.h>
#include <cstdint>

namespace asterion::research {

// Tasks come from the original journal; successful extracts require its verified result.
proto::BatchAssessmentTask capture_batch_assessment_task(
    const task::proto::Task&,
    const std::string& input_digest,
    const task::proto::TaskResponse* verified = nullptr);

// Only the Research owner calls this after verifying its original input/result.
proto::BacktestBatchDefinition capture_backtest_batch(
    const resource::proto::Owner&,
    const proto::BacktestBatchDraft&,
    const task::proto::TaskResponse&,
    const std::string& verified_input_digest,
    const std::string& current_program,
    const std::string& current_risk,
    std::int64_t recorded_at_ms);

} // namespace asterion::research
