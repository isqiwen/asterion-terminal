#pragma once
#include <asterion/data_service.pb.h>
#include <filesystem>
#include <memory>
#include <vector>

namespace asterion::sqlite {

class Database;

} // namespace asterion::sqlite

namespace asterion::data {

class Store;

// Data's durable retention authority. Only its existing serial writer calls this.
// PREPARED and CONFIRMED both keep the immutable source versions alive.
class Retentions
{
public:
    Retentions(std::filesystem::path root, proto::DataOwner owner, const Store&);
    ~Retentions();
    proto::DataRetentionRecord prepare(const proto::DataRetention&);
    proto::DataRetentionRecord confirm(const proto::DataRetention&);
    proto::DataRetentionRecord release(
        const resource::proto::Object&,
        const resource::proto::ReleaseDecision& verified_owner_decision);
    proto::DataRetentionRecord get(const resource::proto::Object&) const;
    std::vector<proto::DataRetentionRecord> holds(const proto::DataVersion&) const;

private:
    proto::DataRetention normalize(const proto::DataRetention&) const;
    std::filesystem::path authority_guard_;
    const Store& store_;
    proto::DataOwner owner_;
    std::unique_ptr<sqlite::Database> ledger_;
};

} // namespace asterion::data
