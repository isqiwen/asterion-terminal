#pragma once
#include <asterion/notice_review.pb.h>
#include <asterion/foundation/serialization.hpp>

namespace asterion::protocol {

void validate_event_set_review_request(const research::proto::EventSetReviewRequest&);
void validate_event_set_review_input(const research::proto::EventSetReviewInput&);
void validate_event_set_review_execution(const research::proto::EventSetReviewExecution&);
void validate_event_set_review_result(
    const research::proto::EventSetReviewInput&,
    const research::proto::EventSetReviewResult&);

Json decode_event_set_review_input(const research::proto::EventSetReviewInput&);
Json decode_event_set_review_result(
    const research::proto::EventSetReviewInput&,
    const research::proto::EventSetReviewResult&);

void validate_notice_source_observation(const research::proto::NoticeSourceObservation&);
Json decode_notice_source_observation(const research::proto::NoticeSourceObservation&);
void validate_notice_review_request(const research::proto::NoticeReviewRequest&);
void validate_notice_review_input(const research::proto::NoticeReviewInput&);
research::proto::NoticeReviewInput make_notice_review_input(
    const resource::proto::Owner&,
    const research::proto::NoticeReviewRequest&,
    const intelligence::proto::NoticePublished&,
    const intelligence::proto::NoticeStoredContent&,
    const std::string& program_artifact);
research::proto::NoticeReviewExecution notice_review_execution(
    const research::proto::NoticeReviewInput&);
void validate_notice_review_execution(const research::proto::NoticeReviewExecution&);
void validate_notice_review_result(
    const research::proto::NoticeReviewInput&,
    const research::proto::NoticeReviewResult&);
Json decode_notice_review_input(const research::proto::NoticeReviewInput&);
Json decode_notice_review_result(
    const research::proto::NoticeReviewInput&,
    const research::proto::NoticeReviewResult&);

} // namespace asterion::protocol
