# Módulo core: tipos comunes, sin dependencia de la interfaz gráfica.
add_library(eu_core STATIC
    src/Error.cpp
    src/FileName.cpp
    src/Log.cpp
    src/NaturalCompare.cpp
    src/SizeFormat.cpp
    src/Version.cpp)
add_library(eu::core ALIAS eu_core)

target_include_directories(eu_core PUBLIC include)
target_link_libraries(eu_core PUBLIC Qt6::Core)
target_compile_definitions(eu_core PRIVATE EU_VERSION_STRING="${PROJECT_VERSION}")
eu_target_defaults(eu_core)
