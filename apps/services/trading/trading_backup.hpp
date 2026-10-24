#pragma once
#include "backup_files.hpp"

namespace asterion::trading {

struct ColdBackup
{
    backup_files::Snapshot snapshot;
    std::vector<resource::proto::Object> objects;
};

ColdBackup
cold_backup(const std::filesystem::path&, const resource::proto::Owner&, std::stop_token = {});

} // namespace asterion::trading
