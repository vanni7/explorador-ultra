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
    # macdeployqt solo arregla el ejecutable principal: el auxiliar busca Qt
    # en los Frameworks del mismo .app.
    set_target_properties(eu-thumbnailer PROPERTIES INSTALL_RPATH "@executable_path/../Frameworks")
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
# De los controladores de bases de datos solo se usa SQLite: los demás
# arrastran bibliotecas (ODBC, PostgreSQL, Oracle…) que no hacen falta.
# La plataforma «offscreen» hace falta siempre: el auxiliar de miniaturas
# (eu-thumbnailer) la usa para pintar sin ventana. Sin ella, cada miniatura
# acababa en un aviso de «no Qt platform plugin».
set(eu_deploy_options EXCLUDE_PLUGINS qsqlodbc qsqlpsql qsqloci qsqlibase qsqlmimer qsqlmysql)
if(WIN32)
    list(APPEND eu_deploy_options INCLUDE_PLUGINS qoffscreen)
elseif(UNIX AND NOT APPLE)
    list(APPEND eu_deploy_options INCLUDE_PLUGINS qwayland qoffscreen EXCLUDE_PLUGIN_TYPES qmltooling)
endif()
if(APPLE)
    # macdeployqt no tiene cómo pedirla: se copia dentro del .app.
    install(FILES "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/platforms/libqoffscreen.dylib"
            DESTINATION "explorador-ultra.app/Contents/PlugIns/platforms")
endif()
qt_generate_deploy_app_script(
    TARGET explorador-ultra
    OUTPUT_SCRIPT eu_deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
    ${eu_deploy_options})
install(SCRIPT "${eu_deploy_script}")

# ------------------------------------------------------------------ paquetes
# CPack hace el .deb (Linux) y el .dmg (macOS); el instalador de Windows lo
# hace Inno Setup (packaging/windows) y el AppImage, linuxdeploy (ver
# docs/DISTRIBUCION.md). Todos parten de la instalación de arriba.
set(CPACK_PACKAGE_NAME "explorador-ultra")
set(CPACK_PACKAGE_VENDOR "Explorador Ultra")
set(CPACK_PACKAGE_CONTACT "https://github.com/vanni7/explorador-ultra")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${PROJECT_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/vanni7/explorador-ultra")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_STRIP_FILES ON)

option(EU_PACKAGE_DEB "Preparar la instalación para el .deb (en /opt, con enlaces en /usr)" OFF)
if(EU_PACKAGE_DEB)
    # En /opt: con prefijo /usr, el Qt que va dentro pisaría al de Ubuntu.
    set(CPACK_GENERATOR DEB)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/explorador-ultra")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_SECTION utils)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Explorador Ultra <https://github.com/vanni7/explorador-ultra>")
    # Las bibliotecas del sistema que hacen falta (libarchive, FFmpeg…) las
    # calcula dpkg-shlibdeps; las de Qt van dentro del paquete.
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS
        "${CMAKE_INSTALL_LIBDIR}" "plugins/platforms" "plugins/imageformats")
    # Para el escritorio: el acceso, los iconos y la orden en el PATH.
    install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/explorador-ultra.desktop"
            DESTINATION /usr/share/applications)
    foreach(size 16 22 24 32 48 64 128 256 512)
        install(FILES "${PROJECT_SOURCE_DIR}/assets/icon/png/explorador-ultra-${size}.png"
                DESTINATION "/usr/share/icons/hicolor/${size}x${size}/apps" RENAME explorador-ultra.png)
    endforeach()
    install(FILES "${PROJECT_SOURCE_DIR}/assets/icon/explorador-ultra.svg"
            DESTINATION /usr/share/icons/hicolor/scalable/apps)
    install(CODE "
        file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/bin\")
        file(CREATE_LINK /opt/explorador-ultra/bin/explorador-ultra
             \"\$ENV{DESTDIR}/usr/bin/explorador-ultra\" SYMBOLIC)")
elseif(APPLE)
    set(CPACK_GENERATOR DragNDrop)
    set(CPACK_DMG_VOLUME_NAME "Explorador Ultra")
endif()
include(CPack)
