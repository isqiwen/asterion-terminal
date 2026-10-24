#pragma once
#include <asterion/intelligence.pb.h>
#include <asterion/foundation/serialization.hpp>

namespace asterion::protocol {

// Transport/identity bounds only. Evidence, mapping and semantic checks are
// the Intelligence rule's responsibility; valid shape does not authorize publish.
void validate_notice_subscription_definition(
    const intelligence::proto::NoticeSubscriptionDefinition&);
void validate_notice_subscription(const intelligence::proto::NoticeSubscriptionRecord&);
void validate_notice_subscription_view(const intelligence::proto::NoticeSubscriptionView&);
void validate_notice_delivery_batch(const intelligence::proto::NoticeDeliveryBatch&);
void validate_notice_reminder(const intelligence::proto::NoticeReminder&);

void validate_notice_delivery(const intelligence::proto::NoticeDelivery&);
void validate_notice_delivery_query(const intelligence::proto::NoticeDeliveryQuery&);
void validate_notice_delivery_page(const intelligence::proto::NoticeDeliveryPage&);
bool notice_delivery_matches(
    const intelligence::proto::NoticeDeliveryFilter&,
    const intelligence::proto::NoticeDelivery&);

void validate_notice_change_request(const intelligence::proto::NoticeChangeRequest&);
void validate_notice_change_input(const intelligence::proto::NoticeChangeInput&);
void validate_notice_change_intent(const intelligence::proto::NoticeChangeIntent&);
void validate_notice_change_record(const intelligence::proto::NoticeChangeRecord&);
void validate_notice_set_request(const intelligence::proto::NoticeSetRequest&);
void validate_notice_set_payload(const intelligence::proto::NoticeSetPayload&);
void validate_notice_set_intent(const intelligence::proto::NoticeSetIntent&);
void validate_notice_set_record(const intelligence::proto::NoticeSetRecord&);
void validate_notice_event_view(const intelligence::proto::NoticeEventView&);
Json decode_notice_event(const intelligence::proto::NoticeEvent&);
Json decode_notice_change(const intelligence::proto::NoticeChangeRecord&);
Json decode_notice_event_view(const intelligence::proto::NoticeEventView&);
void validate_notice_event(const intelligence::proto::NoticeEvent&);
void validate_notice_candidate(const intelligence::proto::NoticeCandidate&);
void validate_notice_input(const intelligence::proto::NoticeInput&);
void validate_notice_assessment(const intelligence::proto::NoticeAssessment&);
void validate_notice_review(const intelligence::proto::NoticeReview&);
void validate_notice_payload(const intelligence::proto::NoticePayload&);
void validate_notice_intent(const intelligence::proto::NoticePublicationIntent&);
void validate_notice_storage(const intelligence::proto::NoticeStorageReceipt&);
void validate_notice_published(const intelligence::proto::NoticePublished&);
void validate_notice_record(const intelligence::proto::NoticeRecord&);
void validate_notice_consumer_object(const resource::proto::Object&);
void validate_notice_consumer_binding(const intelligence::proto::NoticeConsumerBinding&);
void validate_notice_consumer_record(const intelligence::proto::NoticeConsumerRecord&);
std::string notice_consumer_key(const resource::proto::Object&);
Json decode_notice_consumer_record(const intelligence::proto::NoticeConsumerRecord&);

} // namespace asterion::protocol
