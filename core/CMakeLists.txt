# Headers in include/asterion/<module>/, sources and each module's targets in
# src/<module>/. Dependencies point down: foundation, kernel, domain, then the
# database access built on them.
add_subdirectory(src/foundation)
add_subdirectory(src/kernel)
add_subdirectory(src/domain)
add_subdirectory(src/database/sqlite)
