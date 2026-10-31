#pragma once
#include <asterion/material.pb.h>
#include <asterion/base/serialization.hpp>
#include <optional>

namespace asterion::protocol {

inline constexpr std::size_t max_material_bytes = 4 * 1024 * 1024;
void validate_material_request(const data::proto::MaterialRequest&);
void validate_material_receipt(const data::proto::MaterialReceipt&);
void validate_material_input(const data::proto::MaterialInput&);
void validate_material_result(
    const data::proto::MaterialInput&,
    const data::proto::MaterialReceipt&);
Json decode_material_request(const data::proto::MaterialRequest&);
Json decode_material_receipt(const data::proto::MaterialReceipt&);
data::proto::MaterialReceipt freeze_material(
    const resource::proto::Owner&,
    const data::proto::MaterialRequest&,
    std::string_view bytes,
    const std::string& media_type,
    std::int64_t acquired_at_ms,
    std::optional<std::int64_t> source_published_at_ms = {});
// Validates content at its byte-owning boundary. No normalization, OCR or semantic extraction.
data::proto::MaterialText
read_material_text(const data::proto::MaterialReceipt&, std::string_view bytes);
std::string material_excerpt(
    const data::proto::MaterialReceipt&,
    std::string_view original,
    std::size_t begin,
    std::size_t end);

} // namespace asterion::protocol
