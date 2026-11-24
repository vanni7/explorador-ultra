# Distribución

Cómo se hacen, se prueban e instalan los paquetes (SPEC §23, Fase 6).

## Qué se publica
| Sistema | Archivo | Cómo se instala |
|---|---|---|
| Ubuntu 22.04 y 24.04 | `explorador-ultra_<versión>_ubuntu<NN>_amd64.deb` | `sudo apt install ./explorador-ultra_….deb`. Queda en `/opt/explorador-ultra`, con la orden `explorador-ultra` y el acceso en el menú |
| Cualquier Linux de 64 bits (glibc de Ubuntu 22.04 o posterior) | `ExploradorUltra-<versión>-x86_64.AppImage` | Darle permiso de ejecución y abrirlo; no se instala nada |
| Windows 10 y 11 (64 bits) | `ExploradorUltra-<versión>-Windows-x64.exe` | Instalador en español: para todos los usuarios o solo para el actual |
| macOS (Intel y Apple Silicon) | `ExploradorUltra-<versión>-macOS-<arquitectura>.dmg` | Arrastrar la aplicación a Aplicaciones. **Solo compilado en la CI; no verificado en un Mac** |

## Cómo se hacen
Todo parte de `cmake --install` (`cmake/EuPackaging.cmake`), que deja la aplicación con su Qt al lado. El flujo **Paquetes** de GitHub Actions (`.github/workflows/paquetes.yml`) los hace en cada etiqueta `v*`, prueba cada uno arrancando la aplicación ya empaquetada sin el Qt de la compilación, y deja un borrador de versión con los archivos.

- **.deb**: `cmake -DEU_PACKAGE_DEB=ON …` y `cpack`. Va en `/opt` porque con prefijo `/usr` el Qt de dentro pisaría el de Ubuntu. Las dependencias del sistema (libarchive, FFmpeg, LibRaw, Exiv2…) las calcula `dpkg-shlibdeps`, por eso hay un .deb por versión de Ubuntu.
- **AppImage**: la instalación en `AppDir/usr` y `linuxdeploy`, que añade las bibliotecas del sistema que faltan. Se hace en Ubuntu 22.04 para que sirva en sistemas más nuevos.
- **Windows**: `cmake --install` (windeployqt), las DLL de vcpkg y el entorno de C++ de Microsoft, y **Inno Setup** (`packaging/windows/explorador-ultra.iss`). El servicio opcional de NTFS se copia pero no se instala: se instala desde la ventana del índice, y el desinstalador lo quita si quedó instalado.
- **macOS**: `cmake --install` (macdeployqt) y `cpack -G DragNDrop`.

Para sacar una versión: `git tag v0.1.0 && git push origin v0.1.0`, esperar al flujo y publicar el borrador.

## Avisos de seguridad al abrirlo
Los paquetes **no van firmados** (un certificado de firma cuesta dinero). Por eso el sistema avisa la primera vez:

### Windows: SmartScreen
Al abrir el instalador puede salir «Windows protegió su PC». Pulsa **Más información** y luego **Ejecutar de todas formas**. Solo hace falta la primera vez. Si el navegador lo bloquea al descargarlo, elige «Conservar».

### macOS: Gatekeeper
Al abrir la aplicación puede decir que «no se puede abrir porque es de un desarrollador no identificado» o que «está dañada». Opciones:
1. En Finder, clic derecho (o Control+clic) sobre Explorador Ultra → **Abrir** → **Abrir**.
2. O en **Ajustes del Sistema → Privacidad y seguridad**, al final, **Abrir igualmente**.
3. Si dice «está dañada» (pasa con lo descargado de internet): en Terminal, `xattr -dr com.apple.quarantine "/Applications/Explorador Ultra.app"`.

### macOS: Acceso total al disco
macOS no deja ver algunas carpetas (Correo, Mensajes, Safari, copias de Time Machine, otros usuarios) sin permiso. Para explorarlas e indexarlas: **Ajustes del Sistema → Privacidad y seguridad → Acceso total al disco**, pulsar **+**, elegir Explorador Ultra en Aplicaciones y activarlo. Después, cerrar y volver a abrir la aplicación. Sin este permiso todo lo demás funciona igual; esas carpetas se ven vacías o dan «permiso denegado».

### Linux
El .deb no pide nada especial. El AppImage necesita permiso de ejecución (`chmod +x` o Propiedades → Permitir ejecutar). En sistemas sin FUSE: `./ExploradorUltra-….AppImage --appimage-extract-and-run`.
