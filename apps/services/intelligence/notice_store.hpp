#pragma once
#include "program_files.hpp"
#include <asterion/kernel/process/file_lock.hpp>
#include <functional>
#include <stop_token>
#include <asterion/intelligence.pb.h>
#include <asterion/data_service.pb.h>
#include <asterion/foundation/time.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace asterion::intelligence {

class PreparedNotice
{
    friend PreparedNotice prepare_notice(
        const resource::proto::Owner&,
        const proto::NoticeCandidate&,
        const data::proto::MaterialRead&,
        const std::optional<data::proto::TableListedContracts>&,
        const programs::Files::Entry&);
    proto::NoticeInput input_;
    proto::NoticeAssessment assessment_;
    resource::proto::Object reference_;

public:
    const proto::NoticeInput& input() const noexcept
    {
        return input_;
    }

    const proto::NoticeAssessment& assessment() const noexcept
    {
        return assessment_;
    }

    const resource::proto::Object& reference() const noexcept
    {
        return reference_;
    }
};

// Sources already crossed the selected Data client's exact-response boundary.
// No original is copied into Intelligence, and no publication decision is made.
PreparedNotice prepare_notice(
    const resource::proto::Owner&,
    const proto::NoticeCandidate&,
    const data::proto::MaterialRead&,
    const std::optional<data::proto::TableListedContracts>&,
    const programs::Files::Entry&);

class PreparedNoticeChange
{
    friend PreparedNoticeChange prepare_notice_change(
        const resource::proto::Owner&,
        const proto::NoticeChangeRequest&,
        const data::proto::MaterialRead&,
        const programs::Files::Entry&);
    proto::NoticeChangeInput input_;

public:
    const proto::NoticeChangeInput& input() const noexcept
    {
        return input_;
    }
};

PreparedNoticeChange prepare_notice_change(
    const resource::proto::Owner&,
    const proto::NoticeChangeRequest&,
    const data::proto::MaterialRead&,
    const programs::Files::Entry&);

// One Intelligence serial writer. The service owns semantic decisions; this is
// not a task scheduler or a second Data/source library. Reads use this same owner.
class Store
{
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    Store(
        std::filesystem::path,
        resource::proto::Owner,
        std::shared_ptr<const Clock> = std::make_shared<SystemClock>(),
        unsigned max_records = 10000);
    ~Store();
    // Stopped-owner inspection: never opens a writer, repairs a guard or creates a store.
    // The returned shared ownership lock must outlive the caller's cold file cut.
    static std::unique_ptr<FileLock> inspect_backup(
        const std::filesystem::path&,
        const resource::proto::Owner&,
        const std::function<void(const proto::NoticeRecord&)>&,
        std::stop_token = {},
        const std::function<void(const proto::NoticeConsumerRecord&)>& = {},
        const std::function<void(const proto::NoticeChangeRecord&)>& = {},
        const std::function<void(const proto::NoticeSetRecord&)>& = {},
        const std::function<void(const resource::proto::Object&)>& = {});

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    // After the current host's actual program bytes have been retained by Files.
    void set_programme(const programs::Files::Entry&);
    proto::NoticeRecord get(const std::string&) const;
    proto::NoticeDeliveryPage deliveries(const proto::NoticeDeliveryQuery&) const;
    proto::NoticeDeliveryPage preview_subscription(
        const proto::NoticeSubscriptionDefinition&) const;
    proto::NoticeSubscriptionView create_subscription(const proto::NoticeSubscriptionDefinition&);
    proto::NoticeSubscriptionView subscription(const std::string&) const;
    proto::NoticeSubscriptions subscriptions() const;
    proto::NoticeSubscriptionView control_subscription(const proto::NoticeSubscriptionControl&);
    proto::NoticeDeliveryBatch offer_delivery(const proto::NoticeSubscriptionOffer&);
    proto::NoticeDeliveryBatch delivery_batch(const resource::proto::Object&) const;
    proto::NoticeDeliveryBatch delivery_batch(const proto::NoticeDeliveryLookup&) const;
    proto::NoticeSubscriptionView acknowledge_delivery(const resource::proto::Object&);
    proto::NoticeReminderPage reminders(const proto::NoticeReminderQuery&) const;
    resource::proto::Catalogue
    resources(const resource::proto::CatalogueQuery&, std::stop_token = {}) const;
    std::vector<proto::NoticeRecord> list(
        const std::string& after,
        unsigned limit,
        std::optional<proto::NoticeState> state = {}) const;
    proto::NoticeConsumerRecord prepare_consumer(const proto::NoticeConsumerBinding&);
    proto::NoticeConsumerRecord confirm_consumer(const proto::NoticeConsumerBinding&);
    proto::NoticeConsumerRecord consumer(const resource::proto::Object&) const;
    proto::NoticeConsumerPage consumers(const proto::NoticeConsumerQuery&) const;
    // Only the host obtains this proof directly from the exact Research owner.
    proto::NoticeConsumerRecord
    release_consumer(const resource::proto::Object&, const resource::proto::ReleaseDecision&);
    proto::NoticeRecord stage(const PreparedNotice&);
    proto::NoticeRecord
    admit(const resource::proto::Object&, const std::optional<data::proto::DataRetentionRecord>&);
    proto::NoticeVerification review(const proto::NoticeReview&);
    proto::NoticePublicationIntent begin_publication(const proto::NoticePublishRequest&);
    proto::NoticePublished
    confirm_publication(const resource::proto::Object&, const proto::NoticeStorageReceipt&);
    proto::NoticePublicationIntent preparation(const resource::proto::Object&) const;
    proto::NoticePublished publication(const resource::proto::Object&) const;
    proto::NoticeEvent event(const resource::proto::Object&) const;
    proto::NoticeEventView event_view(const proto::NoticeEventQuery&) const;
    // Preview may leave program_artifact empty to learn this host's actual method.
    // Formal publication requires the exact programme returned by that preview.
    proto::NoticeSetPreview preview_set(proto::NoticeSetRequest) const;
    proto::NoticeSetIntent begin_set(const proto::NoticeSetRequest&);
    proto::NoticeSetRecord event_set(const std::string&) const;
    proto::NoticeSetIntent set_preparation(const resource::proto::Object&) const;
    proto::NoticeSetRecord set_publication(const resource::proto::Object&) const;
    proto::NoticeSetRecord
    confirm_set(const resource::proto::Object&, const proto::NoticeStorageReceipt&);
    proto::NoticeChangeIntent begin_change(const PreparedNoticeChange&);
    proto::NoticeChangeRecord change(const std::string&) const;
    proto::NoticeChangeIntent change_preparation(const resource::proto::Object&) const;
    proto::NoticeChangeRecord
    confirm_change(const resource::proto::Object&, const proto::NoticeStorageReceipt&);
    resource::proto::ReleaseDecision abandon(
        const resource::proto::Object&,
        const std::string& operation,
        const std::string& reason);
    resource::proto::ReleaseDecision release_decision(const resource::proto::Object&) const;
};

} // namespace asterion::intelligence
