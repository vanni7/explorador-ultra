# Ejecutable principal. En la Fase 0 es solo una ventana mínima que demuestra
# que Qt compila y arranca en las tres plataformas.
qt_add_executable(explorador-ultra WIN32 MACOSX_BUNDLE
    main.cpp
    resources.qrc)

target_link_libraries(explorador-ultra PRIVATE eu::core eu::tasks eu::ui Qt6::Widgets Qt6::Svg)
eu_target_defaults(explorador-ultra)

set_target_properties(explorador-ultra PROPERTIES
    MACOSX_BUNDLE_BUNDLE_NAME "Explorador Ultra"
    MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.explorador-ultra"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")

# Traducciones (Qt Linguist). El texto del código ya está en español; el archivo
# "es" aporta las formas de singular y plural ("1 elemento", "2 elementos").
qt_add_lrelease(
    TS_FILES ${CMAKE_SOURCE_DIR}/translations/explorador-ultra_es.ts
    QM_FILES_OUTPUT_VARIABLE eu_qm_files)
qt_add_resources(explorador-ultra "translations"
    PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}"
    FILES ${eu_qm_files})
