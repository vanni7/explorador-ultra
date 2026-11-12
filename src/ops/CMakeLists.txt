# Módulo ops: operaciones de archivos con historial y deshacer. Sin interfaz gráfica.
add_library(eu_ops STATIC
    src/FileOps.cpp)
add_library(eu::ops ALIAS eu_ops)

target_include_directories(eu_ops PUBLIC include)
target_link_libraries(eu_ops PUBLIC eu::core eu::tasks eu::fs eu::journal Qt6::Core)
eu_target_defaults(eu_ops)
