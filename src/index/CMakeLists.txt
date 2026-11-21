# Módulo index: índice local de nombres en SQLite (SPEC §6.4) y la vigilancia
# de cambios que lo mantiene al día. Sin interfaz gráfica.
add_library(eu_index STATIC
    src/ChangeJournal.cpp
    src/Index.cpp
    src/IndexService.cpp
    src/TreeWatcher.cpp
    src/ContentIndex.cpp
    src/ContentIndexService.cpp
    include/eu/index/ChangeJournal.h
    include/eu/index/ContentIndex.h
    include/eu/index/ContentIndexService.h
    include/eu/index/Index.h
    include/eu/index/IndexService.h
    include/eu/index/TreeWatcher.h
    include/eu/index/UsnProtocol.h)
add_library(eu::index ALIAS eu_index)

# La vigilancia de un árbol entero es distinta en cada sistema.
if(WIN32)
    # Y el diario de NTFS, a través del servicio opcional eu-indexsvc.
    target_sources(eu_index PRIVATE src/TreeWatcher_win.cpp src/ChangeJournal_win.cpp)
    target_link_libraries(eu_index PRIVATE advapi32 shell32)
elseif(APPLE)
    target_sources(eu_index PRIVATE src/TreeWatcher_mac.cpp)
    target_link_libraries(eu_index PRIVATE "-framework CoreServices")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(eu_index PRIVATE src/TreeWatcher_linux.cpp)
endif()

target_include_directories(eu_index PUBLIC include)
target_link_libraries(eu_index PUBLIC eu::core eu::fs eu::search eu::tasks Qt6::Core PRIVATE Qt6::Sql)
eu_target_defaults(eu_index)
