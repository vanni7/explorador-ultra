# Ejecutable principal. En la Fase 0 es solo una ventana mínima que demuestra
# que Qt compila y arranca en las tres plataformas.
qt_add_executable(explorador-ultra WIN32 MACOSX_BUNDLE
    main.cpp
    resources.qrc)

target_link_libraries(explorador-ultra PRIVATE eu::core Qt6::Widgets Qt6::Svg)
eu_target_defaults(explorador-ultra)

set_target_properties(explorador-ultra PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Explorador Ultra"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.explorador-ultra"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
