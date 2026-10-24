#pragma once
#include <asterion/research_batch.pb.h>
#include <asterion/backtest.pb.h>
#include <asterion/task.pb.h>
#include <asterion/foundation/serialization.hpp>
#include <string_view>

namespace asterion::protocol {

void validate_backtest_batch_draft(const research::proto::BacktestBatchDraft&);
void validate_backtest_batch_definition(const research::proto::BacktestBatchDefinition&);
void validate_backtest_batch_state(const research::proto::BacktestBatchState&);
void validate_backtest_batch_mutation(const research::proto::BacktestBatchMutation&);
research::proto::BacktestBatchDraft encode_backtest_batch_draft(const Json&);
Json decode_backtest_batch_state(const research::proto::BacktestBatchState&);
Json decode_backtest_batch_reply(
    const task::proto::ResearchBatchReply&,
    const research::proto::BacktestBatchRequest&,
    const resource::proto::Owner&);
backtest::proto::BacktestInput backtest_batch_member_input(
    const backtest::proto::BacktestInput&,
    const research::proto::BacktestBatchMember&);
std::string backtest_batch_digest(research::proto::BacktestBatchDefinition);
resource::proto::Object backtest_batch_object(const research::proto::BacktestBatchDefinition&);
backtest::proto::BacktestReuse backtest_batch_member_reuse(
    const research::proto::BacktestBatchDefinition&,
    std::string_view task_id);

} // namespace asterion::protocol
