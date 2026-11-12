# Explorador Ultra — guía operativa para el agente

Administrador de archivos en C++20 + Qt 6 Widgets (GPL-3.0-or-later). La especificación completa es `docs/SPEC.md`.
**Al iniciar cada sesión lee este archivo y `docs/STATUS.md`; al terminar, actualízalos.**

## Compilar y probar (Ubuntu)
```bash
export EU_QT_DIR=$HOME/Qt/6.12.0/gcc_64        # Qt instalado con aqtinstall
cmake --preset release && cmake --build --preset release
ctest --preset release
./build/release/bin/explorador-ultra         # añade --smoke-test para abrir y cerrar
```
Otros presets: `debug`, `asan` (ASan+UBSan), `tsan`.

## Medir
```bash
./build/release/bin/eu-bench mktree <carpeta-temporal> 100000
./build/release/bin/eu-bench listing|sort <carpeta>
./build/release/bin/eu-bench copy <archivo> <carpetaDestino>
```
Los resultados solo se anotan en `docs/BENCHMARKS.md` si se midieron de verdad.

## Convenciones
- Código, nombres de API en inglés; comentarios y documentación en español explicando el *porqué*.
- El dueño del proyecto no programa en C++: código explícito, sin trucos de plantillas, RAII y punteros inteligentes.
- Un módulo = una biblioteca CMake en `src/<módulo>` con cabeceras públicas en `include/eu/<módulo>/`, espacio de nombres `eu::<módulo>` y pruebas en `tests/<módulo>/`.
- Ningún módulo de núcleo enlaza Qt Widgets/Gui; solo `ui` y `app`.
- El hilo de interfaz nunca hace E/S de disco.
- Pruebas con archivos: solo en directorios temporales (`QTemporaryDir`). Para borrar carpetas temporales en la terminal, usa rutas literales, nunca variables.
- Decisiones técnicas → `docs/DECISIONS.md` (ADR). Herramientas instaladas → `docs/ENVIRONMENT.md`.
- Commits pequeños, en ramas por fase (`fase-1/...`), y fusión a `main` solo con la CI en verde.

## Entorno
- Equipo de desarrollo: Ubuntu 26.04 (los destinos oficiales 22.04 y 24.04 se cubren en la CI).
- VM de Windows del proyecto: `EU-Win11-Build` en `~/VMs` (VirtualBox). No tocar ninguna otra VM del equipo.
- macOS: solo GitHub Actions.

## Estado
Fase 1 en curso en la rama `fase-1/core` (repositorio: github.com/vanni7/explorador-ultra). Hechos: `core`, `tasks`, `fs` y la interfaz navegable (`ui`). **Siguiente:** `journal` → `ops` → `trash`, y después conectar las operaciones (copiar, mover, renombrar, eliminar, deshacer) a la interfaz; luego tamaño de carpetas. Detalle en `docs/STATUS.md`.

## Notas útiles
- Captura de la ventana sin pantalla: `XAUTHORITY=$(ls /run/user/1000/.mutter-Xwaylandauth.*) QT_QPA_PLATFORM=xcb ./build/release/bin/explorador-ultra --captura salida.png <carpeta>`.
- Tras añadir textos con `%n`: ejecutar `lupdate -pluralonly ...` y `python3 tools/fill_plurals.py` (instrucciones dentro del guion).
- Qt para Windows no se instala con aqtinstall 3.3.0: usar `tools/windows/fetch_qt_windows.py` (la CI ya lo hace).
- Los objetos de interfaz que lanzan tareas no deben usar `this` en el hilo de trabajo: capturar un `QPointer` y volver al hilo de interfaz con `QMetaObject::invokeMethod(qApp, ...)` (ver `Sidebar.cpp`).
