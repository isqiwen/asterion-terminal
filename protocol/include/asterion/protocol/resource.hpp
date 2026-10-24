#pragma once
#include <asterion/resource.pb.h>
#include <asterion/foundation/serialization.hpp>

namespace asterion::protocol {

unsigned catalogue_limit(const resource::proto::CatalogueQuery&);
Json decode_resource_catalogue(
    const resource::proto::Catalogue&,
    const resource::proto::CatalogueQuery&,
    const resource::proto::Owner&);

} // namespace asterion::protocol
