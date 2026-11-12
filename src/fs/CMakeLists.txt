# Módulo fs: listado de carpetas, unidades y vigilancia de cambios. Sin interfaz gráfica.
add_library(eu_fs STATIC
    src/DirectoryLister.cpp
    src/DirectoryWatcher.cpp
    src/Volumes.cpp
    include/eu/fs/DirectoryWatcher.h
    include/eu/fs/Volumes.h)
add_library(eu::fs ALIAS eu_fs)

if(WIN32)
    target_sources(eu_fs PRIVATE src/DirectoryLister_win.cpp)
else()
    target_sources(eu_fs PRIVATE src/DirectoryLister_posix.cpp)
endif()

target_include_directories(eu_fs PUBLIC include)
target_link_libraries(eu_fs PUBLIC eu::core eu::tasks Qt6::Core)
eu_target_defaults(eu_fs)
