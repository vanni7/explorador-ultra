# Módulo thumbs: miniaturas (SPEC §5.5) y su caché en memoria y en disco.
# Usa QImage (Qt Gui) pero no Widgets.
add_library(eu_thumbs STATIC
    src/Thumbnail.cpp
    src/ThumbnailCache.cpp
    src/ThumbnailService.cpp
    include/eu/thumbs/Thumbnail.h
    include/eu/thumbs/ThumbnailCache.h
    include/eu/thumbs/ThumbnailService.h)
add_library(eu::thumbs ALIAS eu_thumbs)

target_include_directories(eu_thumbs PUBLIC include)
target_link_libraries(eu_thumbs PUBLIC eu::core eu::tasks eu::fs Qt6::Gui)
eu_target_defaults(eu_thumbs)
