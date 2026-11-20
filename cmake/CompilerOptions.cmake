function(asterion_target_defaults target)
  # Designated initializers omit members on purpose; Clang accepts that and
  # GCC's -Wextra would reject it. GCC 15, optimizing, takes the object behind
  # a structured binding of a co_await, and members an aggregate initializer
  # leaves to their defaults, for maybe uninitialized; earlier versions do not
  # and keep the check.
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror
    $<$<CXX_COMPILER_ID:GNU>:-Wno-missing-field-initializers>
    $<$<AND:$<CXX_COMPILER_ID:GNU>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,15>>:-Wno-maybe-uninitialized>)
endfunction()
