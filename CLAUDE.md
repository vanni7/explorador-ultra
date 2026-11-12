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

## Estado (pausa del 2026-11-11)
Fase 1 en curso en la rama `fase-1/core` (repositorio: github.com/vanni7/explorador-ultra), sin fusionar a `main`.

**Hecho:** `core`, `tasks`, `fs`, interfaz navegable (`ui`), `journal`, `ops` (operaciones elementales), `trash` y el motor `ops::Engine` (crear, renombrar, copiar, mover, papelera, eliminar, deshacer, recuperación). Todo con pruebas en verde en local.

**Siguiente paso exacto:**
1. `gh run list --limit 3` y revisar la CI del último envío: `journal`, `ops`, `trash` y el motor nunca se han compilado en la CI de Windows ni macOS. Arreglar lo que falle (candidatos: `renamex_np` en macOS, `MoveFileExW` y rutas en Windows, la prueba `test_ops_engine` que lanza `eu-crash-helper` — en Windows el ejecutable lleva `.exe`).
2. Conectar el motor a la interfaz: clase `eu::ui::FileActions` (QObject) que ejecute `ops::Engine` en tareas `Category::Transfer` y devuelva el `Outcome` al hilo de interfaz con `QPointer` + `invokeMethod(qApp, …)`. Abrir el historial en `main.cpp` (`AppDataLocation/historial.db`) y pasarlo a `MainWindow`.
3. En `BrowserPane`/`MainWindow`: menú contextual y atajos (F2, Supr, Mayús+Supr con confirmación reforzada, Ctrl+C/X/V, Ctrl+Z, Ctrl+Mayús+N), renombrado en línea, portapapeles con `text/uri-list`, arrastrar y soltar, indicador de progreso con cancelar, y mensajes de error del `Outcome`.
4. Diálogo de recuperación al arrancar si `Journal::incompleteOperations()` no está vacío (Revertir / Descartar; "Reanudar" deshabilitado con "próximamente").
5. Vista de la papelera (`trash::listTrash`, restaurar, eliminar, vaciar) y tamaño de carpetas en segundo plano.
6. Verificación manual en Ubuntu y en la VM de Windows, fusionar a `main` con la CI en verde e informe de cierre de la Fase 1.

Detalle por función en `docs/STATUS.md`.

## Notas útiles
- Captura de la ventana sin pantalla: `XAUTHORITY=$(ls /run/user/1000/.mutter-Xwaylandauth.*) QT_QPA_PLATFORM=xcb ./build/release/bin/explorador-ultra --captura salida.png <carpeta>`.
- Tras añadir textos con `%n`: ejecutar `lupdate -pluralonly ...` y `python3 tools/fill_plurals.py` (instrucciones dentro del guion).
- Qt para Windows no se instala con aqtinstall 3.3.0: usar `tools/windows/fetch_qt_windows.py` (la CI ya lo hace).
- El motor de operaciones se compila como `eu_ops_engine` dentro de `src/trash/CMakeLists.txt` (necesita `eu_ops` y `eu_trash`); su código está en `src/ops`.
- Compilar y probar en la VM de Windows: arrancar `EU-Win11-Build` y ejecutar `tools/windows/build.cmd` con `VBoxManage guestcontrol` (ver `docs/ENVIRONMENT.md`); `tools/windows/run-test.cmd <prueba>` muestra la salida de una sola.
- TSan no ejecuta las pruebas con ventana (avisos falsos dentro de Qt y libdbus).
- Los objetos de interfaz que lanzan tareas no deben usar `this` en el hilo de trabajo: capturar un `QPointer` y volver al hilo de interfaz con `QMetaObject::invokeMethod(qApp, ...)` (ver `Sidebar.cpp`).
