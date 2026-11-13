# Módulo index: índice local de nombres en SQLite (SPEC §6.4) y la vigilancia
# de cambios que lo mantiene al día. Sin interfaz gráfica.
add_library(eu_index STATIC
    src/Index.cpp
    src/TreeWatcher.cpp
    include/eu/index/Index.h
    include/eu/index/TreeWatcher.h)
add_library(eu::index ALIAS eu_index)

# La vigilancia de un árbol entero es distinta en cada sistema.
if(WIN32)
    target_sources(eu_index PRIVATE src/TreeWatcher_win.cpp)
elseif(APPLE)
    target_sources(eu_index PRIVATE src/TreeWatcher_mac.cpp)
    target_link_libraries(eu_index PRIVATE "-framework CoreServices")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(eu_index PRIVATE src/TreeWatcher_linux.cpp)
endif()

target_include_directories(eu_index PUBLIC include)
target_link_libraries(eu_index PUBLIC eu::core eu::fs eu::search eu::tasks Qt6::Core PRIVATE Qt6::Sql)
eu_target_defaults(eu_index)
