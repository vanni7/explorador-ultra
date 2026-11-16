# Módulo content: el texto de los documentos, para buscar en ellos (SPEC §6.5).
add_library(eu_content STATIC
    src/TextExtractor.cpp
    include/eu/content/TextExtractor.h)
add_library(eu::content ALIAS eu_content)

target_include_directories(eu_content PUBLIC include)
target_link_libraries(eu_content PUBLIC eu::core eu::tasks Qt6::Core)
eu_target_defaults(eu_content)

if(EU_LIBARCHIVE_FOUND)
    target_link_libraries(eu_content PRIVATE PkgConfig::EU_LIBARCHIVE)
    target_compile_definitions(eu_content PRIVATE EU_HAVE_LIBARCHIVE)
endif()
# Qt PDF (opcional) para el texto de los PDF.
if(TARGET Qt6::Pdf)
    target_link_libraries(eu_content PRIVATE Qt6::Pdf)
    target_compile_definitions(eu_content PRIVATE EU_HAVE_QTPDF)
endif()
