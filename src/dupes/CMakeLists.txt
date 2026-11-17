# Módulo dupes: duplicados exactos por etapas (SPEC §14.1). Solo lee.
add_library(eu_dupes STATIC
    src/Duplicates.cpp
    include/eu/dupes/Duplicates.h)
add_library(eu::dupes ALIAS eu_dupes)

target_include_directories(eu_dupes PUBLIC include)
target_link_libraries(eu_dupes PUBLIC eu::core eu::tasks eu::search eu::ops Qt6::Core)
eu_target_defaults(eu_dupes)
