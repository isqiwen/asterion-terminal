# A service's setup.sh: its own scripts/setup.sh followed by the shared part.
function(asterion_setup_script target)
  set(output ${CMAKE_CURRENT_BINARY_DIR}/setup.sh)
  set(own ${CMAKE_CURRENT_SOURCE_DIR}/scripts/setup.sh)
  set(shared ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scripts/setup.sh)
  add_custom_command(OUTPUT ${output}
    COMMAND sh -c "cat \"$1\" \"$2\" > \"$3\" && chmod 755 \"$3\"" setup ${own} ${shared} ${output}
    DEPENDS ${own} ${shared}
    VERBATIM)
  add_custom_target(${target} ALL DEPENDS ${output})
endfunction()

add_subdirectory(market/ctp)
add_subdirectory(storage/parquet)
