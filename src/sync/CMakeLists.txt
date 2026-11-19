# Módulo sync: comparar dos carpetas y planear su sincronización (SPEC §16).
# Solo lee y planea; aplicar el plan lo hace ops::Engine::synchronize.
add_library(eu_sync STATIC
    src/Sync.cpp
    include/eu/sync/Sync.h)
add_library(eu::sync ALIAS eu_sync)

target_include_directories(eu_sync PUBLIC include)
target_link_libraries(eu_sync PUBLIC eu::core eu::tasks eu::fs eu::ops Qt6::Core)
eu_target_defaults(eu_sync)
