#pragma once
#include "backup_files.hpp"
#include <asterion/data_service.pb.h>

namespace asterion::data {

struct BackupReference
{
    resource::proto::Object consumer;
    std::string retention_digest;
};

struct ColdBackup
{
    backup_files::Snapshot snapshot;
    std::vector<BackupReference> references;
    std::vector<resource::proto::Object> objects;
};

// No process control or provider access. Existing Data ownership must be idle.
ColdBackup cold_backup(const std::filesystem::path&, const proto::DataOwner&, std::stop_token = {});

} // namespace asterion::data
