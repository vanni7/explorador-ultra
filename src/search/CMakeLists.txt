# Módulo search: analizador y evaluador de consultas (SPEC §6.1) y búsqueda
# en vivo recorriendo carpetas (SPEC §6.2). Sin interfaz gráfica.
add_library(eu_search STATIC
    src/Query.cpp
    src/Searcher.cpp
    include/eu/search/Query.h
    include/eu/search/Searcher.h)
add_library(eu::search ALIAS eu_search)

target_include_directories(eu_search PUBLIC include)
target_link_libraries(eu_search PUBLIC eu::core eu::fs Qt6::Core)
eu_target_defaults(eu_search)
