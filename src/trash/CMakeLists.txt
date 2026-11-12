# Módulo trash: papelera propia por volumen (SPEC §10). Sin interfaz gráfica.
add_library(eu_trash STATIC
    src/Trash.cpp)
add_library(eu::trash ALIAS eu_trash)

target_include_directories(eu_trash PUBLIC include)
target_link_libraries(eu_trash PUBLIC eu::core eu::tasks eu::fs eu::ops Qt6::Core)
eu_target_defaults(eu_trash)

# El motor de operaciones (espacio de nombres eu::ops) usa tanto las operaciones
# elementales de eu_ops como la papelera, así que se compila aquí, después de
# ambas, como biblioteca aparte. Su código vive en src/ops.
add_library(eu_ops_engine STATIC
    ${CMAKE_SOURCE_DIR}/src/ops/src/Engine.cpp)
add_library(eu::ops_engine ALIAS eu_ops_engine)
target_link_libraries(eu_ops_engine PUBLIC eu::ops eu::trash eu::journal eu::fs)
eu_target_defaults(eu_ops_engine)
