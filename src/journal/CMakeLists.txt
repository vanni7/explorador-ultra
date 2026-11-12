# Módulo journal: historial persistente de operaciones (SPEC §9). Sin interfaz gráfica.
add_library(eu_journal STATIC
    src/Journal.cpp)
add_library(eu::journal ALIAS eu_journal)

target_include_directories(eu_journal PUBLIC include)
target_link_libraries(eu_journal PUBLIC eu::core Qt6::Core PRIVATE Qt6::Sql)
eu_target_defaults(eu_journal)
