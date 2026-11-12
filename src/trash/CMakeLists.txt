# Módulo trash: papelera propia por volumen (SPEC §10). Sin interfaz gráfica.
add_library(eu_trash STATIC
    src/Trash.cpp)
add_library(eu::trash ALIAS eu_trash)

target_include_directories(eu_trash PUBLIC include)
target_link_libraries(eu_trash PUBLIC eu::core eu::tasks eu::fs eu::ops Qt6::Core)
eu_target_defaults(eu_trash)
