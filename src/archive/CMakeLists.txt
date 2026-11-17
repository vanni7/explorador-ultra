# Módulo archive: archivos comprimidos (SPEC §15). Sin interfaz gráfica.
# Necesita libarchive (cmake/EuOptionalDeps.cmake); sin ella compila igual y
# cada función responde "no disponible".
add_library(eu_archive STATIC
    src/Archive.cpp
    include/eu/archive/Archive.h)
add_library(eu::archive ALIAS eu_archive)

target_include_directories(eu_archive PUBLIC include)
target_link_libraries(eu_archive PUBLIC eu::core eu::tasks Qt6::Core)
eu_target_defaults(eu_archive)

if(EU_LIBARCHIVE_FOUND)
    target_link_libraries(eu_archive PUBLIC PkgConfig::EU_LIBARCHIVE)
    target_compile_definitions(eu_archive PUBLIC EU_HAVE_LIBARCHIVE)
endif()
