# The lab's backtest engine, a Python extension for the bundled runtime. The
# runtime's preparation names that interpreter (scripts/desktop/python_runtime.py
# sets ASTERION_PYTHON_HOME); a build without it has no engine to build.
if(NOT ASTERION_PYTHON_HOME)
  return()
endif()
set(Python_EXECUTABLE "${ASTERION_PYTHON_HOME}/bin/python3.12")
set(PYBIND11_FINDPYTHON ON)
find_package(Python 3.12 EXACT REQUIRED COMPONENTS Interpreter Development.Module)
find_package(pybind11 CONFIG REQUIRED)
pybind11_add_module(asterion_engine engine.cpp)
target_link_libraries(asterion_engine PRIVATE Asterion::Futures)
set_target_properties(asterion_engine PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/engine-python")
asterion_target_defaults(asterion_engine)
