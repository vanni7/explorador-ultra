# Módulo organize: ordenar una carpeta en subcarpetas según reglas (SPEC §12).
add_library(eu_organize STATIC
    src/Organize.cpp
    include/eu/organize/Organize.h)
add_library(eu::organize ALIAS eu_organize)

target_include_directories(eu_organize PUBLIC include)
target_link_libraries(eu_organize PUBLIC eu::core eu::tasks eu::rename Qt6::Core)
eu_target_defaults(eu_organize)
