# Headers in include/asterion/<module>/, sources and each module's targets in
# src/<module>/. Dependencies point down: base, system, futures, then the
# database access built on them.
add_subdirectory(src/base)
add_subdirectory(src/system)
add_subdirectory(src/futures)
add_subdirectory(src/database/sqlite)
