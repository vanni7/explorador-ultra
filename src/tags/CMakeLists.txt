# Módulo tags: etiquetas y colores en una base local (SPEC §17).
add_library(eu_tags STATIC
    src/TagStore.cpp
    include/eu/tags/TagStore.h)
add_library(eu::tags ALIAS eu_tags)

target_include_directories(eu_tags PUBLIC include)
target_link_libraries(eu_tags PUBLIC eu::core eu::tasks eu::fs eu::search eu::journal Qt6::Core PRIVATE Qt6::Sql)
eu_target_defaults(eu_tags)
