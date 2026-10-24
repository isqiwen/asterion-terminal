#pragma once
#include <asterion/account_review.pb.h>
#include <asterion/foundation/serialization.hpp>

namespace asterion::protocol {

void validate_account_review_request(const research::proto::AccountReviewRequest&);
void validate_account_review_input(const research::proto::AccountReviewInput&);
research::proto::AccountReviewInput make_account_review_input(
    const resource::proto::Owner&,
    const research::proto::AccountReviewRequest&,
    const proto::TradingFactsStatus&,
    const std::string& program_artifact);
void validate_account_review_execution(const research::proto::AccountReviewExecution&);
void validate_account_review_result(
    const research::proto::AccountReviewInput&,
    const research::proto::AccountReviewResult&);
research::proto::AccountReviewExecution account_review_execution(
    const research::proto::AccountReviewInput&);
Json decode_account_review_input(const research::proto::AccountReviewInput&);
Json decode_account_review_result(
    const research::proto::AccountReviewInput&,
    const research::proto::AccountReviewResult&);

} // namespace asterion::protocol
