# Servicio opcional de Windows que lee el diario de cambios y la MFT de NTFS
# (SPEC §6.4, ADR-0013). Sin Qt: corre como LocalSystem, así que es un único
# .exe con el entorno de ejecución de C++ dentro (/MT) y nada más que cargar.
add_executable(eu-indexsvc main.cpp)
target_include_directories(eu-indexsvc PRIVATE ${PROJECT_SOURCE_DIR}/src/index/include)
target_link_libraries(eu-indexsvc PRIVATE advapi32 shell32 ole32)
set_property(TARGET eu-indexsvc PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
eu_target_defaults(eu-indexsvc)
