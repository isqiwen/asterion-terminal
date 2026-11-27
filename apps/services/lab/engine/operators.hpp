#pragma once

#include <pybind11/pybind11.h>

namespace asterion {

void register_operators(pybind11::module_& module);

} // namespace asterion
