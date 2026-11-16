# Módulo meta: metadatos por tipo de archivo (SPEC §7.2). Sin interfaz gráfica.
add_library(eu_meta STATIC
    src/Metadata.cpp
    include/eu/meta/Metadata.h)
add_library(eu::meta ALIAS eu_meta)

target_include_directories(eu_meta PUBLIC include)
target_link_libraries(eu_meta PUBLIC eu::core eu::tasks Qt6::Gui)
eu_target_defaults(eu_meta)

# Bibliotecas opcionales (cmake/EuOptionalDeps.cmake).
if(EU_FFMPEG_FOUND)
    target_link_libraries(eu_meta PRIVATE PkgConfig::EU_FFMPEG)
    target_compile_definitions(eu_meta PRIVATE EU_HAVE_FFMPEG)
endif()
if(EU_EXIV2_FOUND)
    # Público: las pruebas lo usan para escribir EXIF en una foto de prueba.
    target_link_libraries(eu_meta PUBLIC PkgConfig::EU_EXIV2)
    target_compile_definitions(eu_meta PUBLIC EU_HAVE_EXIV2)
endif()
