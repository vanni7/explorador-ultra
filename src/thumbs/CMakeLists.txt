# Módulo thumbs: miniaturas (SPEC §5.5) y su caché en memoria y en disco.
# Usa QImage (Qt Gui) pero no Widgets.
add_library(eu_thumbs STATIC
    src/MediaThumbnail.cpp
    src/MediaThumbnail.h
    src/OfficeThumbnail.cpp
    src/OfficeThumbnail.h
    src/Isolated.cpp
    src/Thumbnail.cpp
    src/ThumbnailCache.cpp
    src/ThumbnailService.cpp
    include/eu/thumbs/Isolated.h
    include/eu/thumbs/Thumbnail.h
    include/eu/thumbs/ThumbnailCache.h
    include/eu/thumbs/ThumbnailService.h)
add_library(eu::thumbs ALIAS eu_thumbs)

target_include_directories(eu_thumbs PUBLIC include)
target_link_libraries(eu_thumbs PUBLIC eu::core eu::tasks eu::fs eu::meta eu::content Qt6::Gui)
eu_target_defaults(eu_thumbs)

# Bibliotecas opcionales: detectadas en cmake/EuOptionalDeps.cmake.
if(EU_FFMPEG_FOUND)
    target_link_libraries(eu_thumbs PUBLIC PkgConfig::EU_FFMPEG)
    target_compile_definitions(eu_thumbs PUBLIC EU_HAVE_FFMPEG)
endif()
if(EU_LIBARCHIVE_FOUND)
    # Público: las pruebas lo usan para fabricar documentos de prueba.
    target_link_libraries(eu_thumbs PUBLIC EU::libarchive)
    target_compile_definitions(eu_thumbs PUBLIC EU_HAVE_LIBARCHIVE)
endif()
if(EU_LIBRAW_FOUND)
    target_link_libraries(eu_thumbs PRIVATE PkgConfig::EU_LIBRAW)
    target_compile_definitions(eu_thumbs PUBLIC EU_HAVE_LIBRAW)
endif()

# El proceso auxiliar que decodifica de forma aislada (Isolated.h). Va junto
# al ejecutable principal (build/<preset>/bin).
qt_add_executable(eu-thumbnailer helper/main.cpp)
set_target_properties(eu-thumbnailer PROPERTIES WIN32_EXECUTABLE FALSE MACOSX_BUNDLE FALSE)
target_link_libraries(eu-thumbnailer PRIVATE eu::thumbs Qt6::Gui)
eu_target_defaults(eu-thumbnailer)
