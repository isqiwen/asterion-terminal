add_library(asterion_paper STATIC paper_execution.cpp replay_schedule.cpp portfolio.cpp)
target_link_libraries(asterion_paper PUBLIC Asterion::Domain asterion_protocol)
target_include_directories(asterion_paper PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
asterion_target_defaults(asterion_paper)
