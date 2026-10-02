#pragma once
#include <asterion/protocol/trading.hpp>
#include <filesystem>

namespace asterion::trading {
// Shared by recovery and read-only provenance inspection. Bump for changes
// to matching, account arithmetic, risk or command replay semantics.
inline const std::string journal_engine = "asterion.paper-futures.v7";
inline constexpr int journal_format = 5;
void validate_paper_header(const Json& header);
// Input only: does not load plugins or validate/replay the command log.
protocol::v1::PaperInput read_paper_input(const std::filesystem::path& directory);
} // namespace asterion::trading
