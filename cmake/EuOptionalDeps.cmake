# Bibliotecas opcionales de la Fase 3 (DEPENDENCIES.md). Si falta alguna, la
# aplicación compila igual y esa función queda desactivada (se ve el icono
# del tipo de archivo en lugar de la miniatura, o faltan esos metadatos).
#
# Define, si las encuentra, los objetivos PkgConfig::EU_FFMPEG,
# PkgConfig::EU_LIBARCHIVE, PkgConfig::EU_LIBRAW y PkgConfig::EU_EXIV2, y las
# variables EU_<nombre>_FOUND.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    # Video y audio. 6.1 o posterior por la API de datos laterales que se usa
    # (Ubuntu 22.04 trae la 4.4: allí no hay miniaturas de video).
    pkg_check_modules(EU_FFMPEG QUIET IMPORTED_TARGET GLOBAL
        libavformat>=60.16 libavcodec>=60.31 libswscale>=7.5 libavutil>=58.29)
    # Leer por dentro los documentos de oficina (son ZIP).
    pkg_check_modules(EU_LIBARCHIVE QUIET IMPORTED_TARGET GLOBAL libarchive>=3.6)
    # Fotos RAW, en la variante de LibRaw segura entre hilos.
    pkg_check_modules(EU_LIBRAW QUIET IMPORTED_TARGET GLOBAL libraw_r>=0.20)
    # EXIF, XMP e IPTC. 0.28 o posterior: la 0.27 (Ubuntu 22.04 y 24.04) tiene
    # otra API (std::auto_ptr); allí no hay EXIF en el panel.
    pkg_check_modules(EU_EXIV2 QUIET IMPORTED_TARGET GLOBAL exiv2>=0.28)
endif()
message(STATUS "Opcionales: FFmpeg=${EU_FFMPEG_FOUND} libarchive=${EU_LIBARCHIVE_FOUND} "
               "LibRaw=${EU_LIBRAW_FOUND} Exiv2=${EU_EXIV2_FOUND}")
