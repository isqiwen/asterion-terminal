add_library(asterion_ctp STATIC ctp_feed.cpp ctp_catalog.cpp)
target_include_directories(asterion_ctp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(asterion_ctp PUBLIC Asterion::Domain PRIVATE ctp::ctp Iconv::Iconv ${CMAKE_DL_LIBS})
asterion_target_defaults(asterion_ctp)
