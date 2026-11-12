# Herramientas de medición (SPEC §4 y §22). No forman parte de la aplicación.
qt_add_executable(eu-bench bench_main.cpp)
target_link_libraries(eu-bench PRIVATE eu::core eu::fs Qt6::Core)
eu_target_defaults(eu-bench)
