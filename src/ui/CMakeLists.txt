# Módulo ui: la interfaz gráfica (Qt Widgets). Es el único, junto con app,
# que puede depender de Qt Widgets.
add_library(eu_ui STATIC
    src/BrowserPane.cpp
    src/DirectoryModel.cpp
    src/EntrySort.cpp
    src/FileActions.cpp
    src/FileClipboard.cpp
    src/IndexDialog.cpp
    src/ContentIndexDialog.cpp
    src/TransfersPanel.cpp
    src/BulkRenameDialog.cpp
    src/OrganizeDialog.cpp
    src/DuplicatesDialog.cpp
    src/SimilarImagesDialog.cpp
    src/TagsDialog.cpp
    src/InlinePlayer.cpp
    src/MainWindow.cpp
    src/MillerView.cpp
    src/PathBar.cpp
    src/PreviewPanel.cpp
    src/SearchResultsModel.cpp
    src/Sidebar.cpp
    src/TrashDialog.cpp
    src/ThumbnailView.cpp
    src/TypeCatalog.cpp
    src/VolumesView.cpp
    include/eu/ui/BrowserPane.h
    include/eu/ui/DirectoryModel.h
    include/eu/ui/FileActions.h
    include/eu/ui/FileClipboard.h
    include/eu/ui/IndexDialog.h
    include/eu/ui/ContentIndexDialog.h
    include/eu/ui/TransfersPanel.h
    include/eu/ui/BulkRenameDialog.h
    include/eu/ui/OrganizeDialog.h
    include/eu/ui/DuplicatesDialog.h
    include/eu/ui/SimilarImagesDialog.h
    include/eu/ui/TagsDialog.h
    include/eu/ui/InlinePlayer.h
    include/eu/ui/Locations.h
    include/eu/ui/MainWindow.h
    include/eu/ui/MillerView.h
    include/eu/ui/PathBar.h
    include/eu/ui/PreviewPanel.h
    include/eu/ui/SearchResultsModel.h
    include/eu/ui/Sidebar.h
    include/eu/ui/TrashDialog.h
    include/eu/ui/ThumbnailView.h
    include/eu/ui/TypeCatalog.h
    include/eu/ui/VolumesView.h)
add_library(eu::ui ALIAS eu_ui)

target_include_directories(eu_ui PUBLIC include)
target_link_libraries(eu_ui PUBLIC eu::core eu::tasks eu::fs eu::search eu::index eu::thumbs eu::meta eu::archive eu::rename eu::organize eu::sync eu::dupes eu::similar eu::tags eu::journal eu::ops_engine Qt6::Widgets)
eu_target_defaults(eu_ui)

# Reproducción dentro de las miniaturas (opcional: sin Qt Multimedia no hay
# botón de reproducir y todo lo demás funciona igual).
if(TARGET Qt6::Multimedia)
    target_link_libraries(eu_ui PRIVATE Qt6::Multimedia)
    target_compile_definitions(eu_ui PRIVATE EU_HAVE_QTMULTIMEDIA)
endif()
