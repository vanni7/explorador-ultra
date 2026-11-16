# Módulo similar: imágenes parecidas por huella perceptual (SPEC §14.2). Solo lee.
add_library(eu_similar STATIC
    src/Similar.cpp
    include/eu/similar/Similar.h)
add_library(eu::similar ALIAS eu_similar)

target_include_directories(eu_similar PUBLIC include)
target_link_libraries(eu_similar PUBLIC eu::core eu::tasks eu::search Qt6::Gui)
eu_target_defaults(eu_similar)
