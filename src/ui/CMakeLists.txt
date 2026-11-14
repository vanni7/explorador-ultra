# Módulo ui: la interfaz gráfica (Qt Widgets). Es el único, junto con app,
# que puede depender de Qt Widgets.
add_library(eu_ui STATIC
    src/BrowserPane.cpp
    src/DirectoryModel.cpp
    src/EntrySort.cpp
    src/FileActions.cpp
    src/FileClipboard.cpp
    src/IndexDialog.cpp
    src/MainWindow.cpp
    src/PathBar.cpp
    src/SearchResultsModel.cpp
    src/Sidebar.cpp
    src/TrashDialog.cpp
    src/TypeCatalog.cpp
    src/VolumesView.cpp
    include/eu/ui/BrowserPane.h
    include/eu/ui/DirectoryModel.h
    include/eu/ui/FileActions.h
    include/eu/ui/FileClipboard.h
    include/eu/ui/IndexDialog.h
    include/eu/ui/Locations.h
    include/eu/ui/MainWindow.h
    include/eu/ui/PathBar.h
    include/eu/ui/SearchResultsModel.h
    include/eu/ui/Sidebar.h
    include/eu/ui/TrashDialog.h
    include/eu/ui/TypeCatalog.h
    include/eu/ui/VolumesView.h)
add_library(eu::ui ALIAS eu_ui)

target_include_directories(eu_ui PUBLIC include)
target_link_libraries(eu_ui PUBLIC eu::core eu::tasks eu::fs eu::search eu::index eu::journal eu::ops_engine Qt6::Widgets)
eu_target_defaults(eu_ui)
