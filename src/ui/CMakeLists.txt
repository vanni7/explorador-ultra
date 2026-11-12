# Módulo ui: la interfaz gráfica (Qt Widgets). Es el único, junto con app,
# que puede depender de Qt Widgets.
add_library(eu_ui STATIC
    src/BrowserPane.cpp
    src/DirectoryModel.cpp
    src/EntrySort.cpp
    src/FileActions.cpp
    src/FileClipboard.cpp
    src/MainWindow.cpp
    src/PathBar.cpp
    src/Sidebar.cpp
    src/TrashDialog.cpp
    include/eu/ui/BrowserPane.h
    include/eu/ui/DirectoryModel.h
    include/eu/ui/FileActions.h
    include/eu/ui/FileClipboard.h
    include/eu/ui/MainWindow.h
    include/eu/ui/PathBar.h
    include/eu/ui/Sidebar.h
    include/eu/ui/TrashDialog.h)
add_library(eu::ui ALIAS eu_ui)

target_include_directories(eu_ui PUBLIC include)
target_link_libraries(eu_ui PUBLIC eu::core eu::tasks eu::fs eu::journal eu::ops_engine Qt6::Widgets)
eu_target_defaults(eu_ui)
