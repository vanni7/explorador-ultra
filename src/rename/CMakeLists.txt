# Módulo rename: renombrado masivo con reglas en cadena y vista previa (SPEC §11).
add_library(eu_rename STATIC
    src/Rename.cpp
    include/eu/rename/Rename.h)
add_library(eu::rename ALIAS eu_rename)

target_include_directories(eu_rename PUBLIC include)
target_link_libraries(eu_rename PUBLIC eu::core Qt6::Core)
eu_target_defaults(eu_rename)
