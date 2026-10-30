#pragma once
#include <asterion/protocol/material.hpp>
#include <filesystem>
#include <functional>
#include <stop_token>

namespace asterion::material_providers {

struct Source
{
    std::string id, name, usage, plugin_id, artifact;
    std::uint32_t max_bytes;
    bool credential_required, no_additional_fee;
};

std::vector<Source> sources();
void preserve(
    const data::proto::MaterialRequest&,
    const std::filesystem::path&,
    std::stop_token = {});

struct Response
{
    std::string bytes, media_type;
    std::int64_t acquired_at_ms;
    std::optional<std::int64_t> source_published_at_ms;
};

// No credentials or implicit requests. Called only after Data task admission;
// acquire is the owner's budget decision immediately before every provider call.
Response fetch(
    const data::proto::MaterialRequest&,
    const std::function<void(std::stop_token)>& acquire,
    std::stop_token = {});
std::filesystem::path artifact_path(const std::string& source, const std::string& expected);

} // namespace asterion::material_providers
