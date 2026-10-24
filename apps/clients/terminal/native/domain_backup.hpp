#pragma once
#include "backup_files.hpp"

namespace asterion::terminal_backup {

struct Selection
{
    resource::proto::Owner owner;
    std::filesystem::path directory;
};

// Local maintenance only: acquire all existing domain ownership locks, then
// validate both sides of fixed references before copying. Never controls services.
backup_files::Cut cold_cut(std::vector<Selection>, std::stop_token = {});

} // namespace asterion::terminal_backup
