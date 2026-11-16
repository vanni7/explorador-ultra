# Módulo thumbs: miniaturas (SPEC §5.5) y su caché en memoria y en disco.
# Usa QImage (Qt Gui) pero no Widgets.
add_library(eu_thumbs STATIC
    src/MediaThumbnail.cpp
    src/MediaThumbnail.h
    src/OfficeThumbnail.cpp
    src/OfficeThumbnail.h
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

# Video y audio con FFmpeg (DEPENDENCIES.md). Es opcional: sin él, o con una
# versión anterior a la 6.1 (Ubuntu 22.04 trae la 4.4), video y audio se ven
# con su icono. La versión mínima es la de la API de datos laterales que se usa.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(EU_FFMPEG QUIET IMPORTED_TARGET
        libavformat>=60.16 libavcodec>=60.31 libswscale>=7.5 libavutil>=58.29)
endif()
if(EU_FFMPEG_FOUND)
    message(STATUS "Miniaturas de video y audio: FFmpeg ${EU_FFMPEG_libavformat_VERSION}")
    target_link_libraries(eu_thumbs PUBLIC PkgConfig::EU_FFMPEG)
    target_compile_definitions(eu_thumbs PUBLIC EU_HAVE_FFMPEG)
else()
    message(STATUS "Miniaturas de video y audio: desactivadas (no hay FFmpeg 6.1 o posterior)")
endif()

# Documentos de oficina: la miniatura que llevan dentro se lee con libarchive
# (también opcional; sin él solo queda LibreOffice, si está instalado).
if(PkgConfig_FOUND)
    pkg_check_modules(EU_LIBARCHIVE QUIET IMPORTED_TARGET libarchive>=3.6)
    # Fotos RAW con LibRaw, en su variante segura entre hilos.
    pkg_check_modules(EU_LIBRAW QUIET IMPORTED_TARGET libraw_r>=0.20)
endif()
if(EU_LIBARCHIVE_FOUND)
    # Público: las pruebas lo usan para fabricar documentos de prueba.
    target_link_libraries(eu_thumbs PUBLIC PkgConfig::EU_LIBARCHIVE)
    target_compile_definitions(eu_thumbs PUBLIC EU_HAVE_LIBARCHIVE)
endif()
if(EU_LIBRAW_FOUND)
    target_link_libraries(eu_thumbs PRIVATE PkgConfig::EU_LIBRAW)
    target_compile_definitions(eu_thumbs PUBLIC EU_HAVE_LIBRAW)
endif()
message(STATUS "Miniaturas de oficina (libarchive): ${EU_LIBARCHIVE_FOUND}; RAW (LibRaw): ${EU_LIBRAW_FOUND}")
