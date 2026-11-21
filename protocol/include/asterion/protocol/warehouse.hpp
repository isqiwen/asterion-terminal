#pragma once
#include <asterion/warehouse.pb.h>
#include <string>
#include <string_view>

namespace asterion::protocol {

std::string table_name(::warehouse::Table table);
::warehouse::Table table_of(std::string_view name);

} // namespace asterion::protocol
