# Módulo search: analizador y evaluador de consultas de búsqueda (SPEC §6.1).
# Sin interfaz gráfica y sin acceso a disco: solo decide si un elemento cumple.
add_library(eu_search STATIC
    src/Query.cpp
    include/eu/search/Query.h)
add_library(eu::search ALIAS eu_search)

target_include_directories(eu_search PUBLIC include)
target_link_libraries(eu_search PUBLIC eu::core eu::fs Qt6::Core)
eu_target_defaults(eu_search)
