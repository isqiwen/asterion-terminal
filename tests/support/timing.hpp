#pragma once
#include <chrono>
// Upper bounds in timing assertions are multiplied under sanitizers, which
// slow execution by up to an order of magnitude. Correctness checks are not.
namespace asterion::testing_support {
#ifdef ASTERION_SANITIZED
inline constexpr int timing_scale = 20;
#else
inline constexpr int timing_scale = 1;
#endif
template <class Duration> constexpr Duration bound(Duration value) {
  return value * timing_scale;
}
} // namespace asterion::testing_support
