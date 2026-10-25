#pragma once
#include "backup_files.hpp"
#include <asterion/data_service.pb.h>

namespace asterion::research {

struct BackupReference
{
    data::proto::DataOwner source;
    resource::proto::Object consumer;
    std::string retention_digest;
    bool released = false;
};

struct ColdBackup
{
    backup_files::Snapshot snapshot;
    std::vector<resource::proto::Object> objects;
    std::vector<BackupReference> references;
};

ColdBackup
cold_backup(const std::filesystem::path&, const resource::proto::Owner&, std::stop_token = {});

} // namespace asterion::research
