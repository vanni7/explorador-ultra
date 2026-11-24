# Instalación y paquetes (SPEC §23, Fase 6). Lo instalado es autosuficiente:
# los programas, Qt con sus complementos y traducciones, y en Linux el acceso
# del menú y los iconos. Los paquetes (.deb, AppImage, instalador de Windows y
# .dmg) se hacen a partir de esta instalación (ver docs/DISTRIBUCION.md).
include(GNUInstallDirs)

# Los programas auxiliares van junto a la aplicación: la aplicación los busca
# en su misma carpeta.
set(eu_installed_programs explorador-ultra eu-thumbnailer)
if(WIN32)
    list(APPEND eu_installed_programs eu-indexsvc)
endif()

if(APPLE)
    install(TARGETS explorador-ultra BUNDLE DESTINATION .)
    # Dentro del .app, junto al ejecutable principal.
    install(TARGETS eu-thumbnailer RUNTIME DESTINATION "explorador-ultra.app/Contents/MacOS")
else()
    install(TARGETS ${eu_installed_programs} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

if(UNIX AND NOT APPLE)
    # Las bibliotecas de Qt que se copian van a ../lib respecto a los programas.
    set_target_properties(explorador-ultra eu-thumbnailer PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/explorador-ultra.desktop"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    foreach(size 16 22 24 32 48 64 128 256 512)
        install(FILES "${PROJECT_SOURCE_DIR}/assets/icon/png/explorador-ultra-${size}.png"
                DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps"
                RENAME explorador-ultra.png)
    endforeach()
    install(FILES "${PROJECT_SOURCE_DIR}/assets/icon/explorador-ultra.svg"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
endif()

install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/explorador-ultra")

# Qt junto a la aplicación: windeployqt en Windows, macdeployqt en macOS y,
# en Linux, las bibliotecas de Qt que el programa usa. En Linux el despliegue
# solo lleva la plataforma X11 (xcb): se añaden Wayland (la sesión normal de
# Ubuntu) y «offscreen» (sin pantalla, para comprobar el paquete en la CI).
# El auxiliar de miniaturas usa un subconjunto de lo que usa la aplicación.
set(eu_deploy_options "")
if(UNIX AND NOT APPLE)
    set(eu_deploy_options INCLUDE_PLUGINS qwayland qoffscreen EXCLUDE_PLUGIN_TYPES qmltooling)
endif()
qt_generate_deploy_app_script(
    TARGET explorador-ultra
    OUTPUT_SCRIPT eu_deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
    ${eu_deploy_options})
install(SCRIPT "${eu_deploy_script}")
