# SQLite access shared by the services that keep their state in SQLite.
add_library(asterion_sqlite STATIC sqlite_database.cpp sqlite_journal.cpp)
target_link_libraries(asterion_sqlite PUBLIC Asterion::System PRIVATE SQLite::SQLite3)
asterion_target_defaults(asterion_sqlite)
