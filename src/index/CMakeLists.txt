# Módulo index: índice local de nombres en SQLite (SPEC §6.4) y la vigilancia
# de cambios que lo mantiene al día. Sin interfaz gráfica.
add_library(eu_index STATIC
    src/Index.cpp
    include/eu/index/Index.h)
add_library(eu::index ALIAS eu_index)

target_include_directories(eu_index PUBLIC include)
target_link_libraries(eu_index PUBLIC eu::core eu::fs eu::search eu::tasks Qt6::Core PRIVATE Qt6::Sql)
eu_target_defaults(eu_index)
