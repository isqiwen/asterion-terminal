#include "paper_record.hpp"
#include "sqlite_journal.hpp"
#include <stdexcept>

namespace asterion::trading {
void validate_paper_header(const Json& header) {
  if (!header.is_object() || !header.contains("format") || header.at("format") != journal_format)
    throw std::invalid_argument("unsupported trading journal format; this build reads format 5 "
                                "only and leaves the directory unchanged");
  require_fields(header, {"format", "engine", "risk_artifact", "manifest"});
  if (header.at("engine") != journal_engine)
    throw std::invalid_argument(
        "trading journal was written by engine " + header.at("engine").dump() +
        " but this build implements " + journal_engine +
        "; recovery refused instead of recomputing history under different rules");
}
protocol::v1::PaperInput read_paper_input(const std::filesystem::path& directory) {
  const auto header = read_journal_header(directory, {"plugins"});
  validate_paper_header(header);
  return protocol::encode_input(header.at("manifest"));
}
} // namespace asterion::trading
