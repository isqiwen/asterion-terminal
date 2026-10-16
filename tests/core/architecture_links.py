"""Exercise the production link guard with small real CMake target graphs."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
base = f'''cmake_minimum_required(VERSION 3.25)
project(ArchitectureProbe NONE)
include("{(root / 'cmake/Architecture.cmake').as_posix()}")
foreach(target asterion_foundation asterion_kernel asterion_domain
    asterion-task-service asterion-node-agent asterion_backtest_engine asterion_factor_engine)
  add_library(${{target}} INTERFACE)
endforeach()
add_library(external INTERFACE IMPORTED)
target_link_libraries(asterion_foundation INTERFACE external)
target_link_libraries(asterion_kernel INTERFACE asterion_foundation)
target_link_libraries(asterion_domain INTERFACE asterion_kernel)
# Packaging order does not put the algorithm in the service's address space.
add_dependencies(asterion-task-service asterion_backtest_engine)
'''
cases = {
    'valid': ('', None),
    'upward': ('target_link_libraries(asterion_foundation INTERFACE "$<LINK_ONLY:asterion_domain>")',
               ('asterion_foundation links upward to asterion_domain',)),
    'task_transitive': ('''add_library(bridge INTERFACE)
add_library(probe::bridge ALIAS bridge)
target_link_libraries(bridge INTERFACE asterion_backtest_engine)
target_link_libraries(asterion-task-service INTERFACE "$<$<CONFIG:Debug>:probe::bridge>")''',
                        ('asterion-task-service must not execute algorithms from', 'asterion_backtest_engine')),
    'agent': ('target_link_libraries(asterion-node-agent INTERFACE asterion_factor_engine)',
              ('asterion-node-agent must not execute algorithms from', 'asterion_factor_engine')),
}
with tempfile.TemporaryDirectory(prefix='asterion-architecture-') as temporary:
    for name, (links, expected) in cases.items():
        source = Path(temporary) / name
        source.mkdir()
        (source / 'CMakeLists.txt').write_text(base + links + '\nasterion_check_architecture()\n')
        result = subprocess.run(['cmake', '-S', str(source), '-B', str(source / 'build')],
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=30)
        if expected is None:
            assert result.returncode == 0, result.stdout
        else:
            assert result.returncode != 0 and all(text in result.stdout for text in expected), result.stdout
        print(f'Architecture link guard: {name} passed')
