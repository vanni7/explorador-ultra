# Registro de decisiones (ADR)

Formato: contexto, decisión, alternativas y consecuencias. Las decisiones previas del usuario están en `SPEC.md` §1.

## ADR-0001 — Qt 6.12 LTS instalado con aqtinstall
- **Fecha:** 2026-11-11 · **Estado:** aceptada
- **Contexto:** la especificación pide la LTS más reciente. Qt 6.12.0 salió el 30-sep-2026 y es LTS (soporte hasta 2031). La LTS anterior, 6.8, solo publica parches para clientes de pago (el último público es 6.8.3, de marzo de 2025). Ubuntu 26.04 trae Qt 6.10.2, que no es LTS, y cada Ubuntu trae una versión distinta.
- **Decisión:** usar Qt 6.12.x de los binarios oficiales, instalados con `aqtinstall` en el equipo y con `install-qt-action` en la CI: la misma versión en Linux, Windows y macOS.
- **Alternativas:** Qt del sistema (versión distinta en cada Ubuntu, imposible igualar con Windows/macOS); Qt 6.8.3 (sin parches públicos de seguridad).
- **Consecuencias:** el `.deb` y el AppImage deben incluir Qt dentro (no pueden depender del Qt de Ubuntu). **Pendiente de verificar en la CI** que Qt 6.12 admite Ubuntu 22.04 y macOS 13; si no, se recurre a 6.8.3 para esas plataformas o se sube el mínimo, con nuevo ADR.

## ADR-0002 — Interfaz con Qt Widgets (no Qt Quick)
- **Fecha:** 2026-11-11 · **Estado:** aceptada
- **Contexto:** hay que elegir entre Widgets clásicos y Qt Quick/QML.
- **Decisión:** Qt Widgets, con el patrón modelo/vista (`QAbstractItemModel` + `QTreeView`/`QListView`).
- **Motivos:**
  1. Las vistas de Widgets ya están virtualizadas y probadas con cientos de miles de filas, con columnas ordenables, cabeceras, edición en línea (F2) y selección extendida: justo lo que pide §5.4.
  2. Aspecto y comportamiento nativos de escritorio: menús contextuales, arrastrar y soltar con otras aplicaciones, accesibilidad, diálogos e impresión (`QPrintPreviewDialog`) vienen hechos.
  3. Un solo lenguaje (C++). Con Quick habría además QML/JavaScript, más difícil de mantener para el dueño del proyecto.
  4. No exige GPU ni controladores gráficos sanos; importa en máquinas virtuales y equipos modestos (el de referencia tiene 8 GB).
  5. Barras de herramientas reorganizables y paneles acoplables (`QToolBar`, `QDockWidget`, `QSplitter`) existen de serie (§5.3).
- **Alternativas:** Qt Quick (animaciones más fluidas y aspecto más moderno "gratis", pero tablas, arrastrar y soltar entre aplicaciones, accesibilidad e impresión requieren mucho más trabajo propio); híbrido con `QQuickWidget` (dos tecnologías que mantener).
- **Consecuencias:** el aspecto moderno se logra con un `QStyle`/hoja de estilos propia y delegados pintados a mano (miniaturas, migas de pan). Las animaciones serán discretas (`QPropertyAnimation`). La reproducción de video en miniaturas usará `QVideoSink` pintando en el delegado.

## ADR-0003 — Origen de las dependencias
- **Fecha:** 2026-11-11 · **Estado:** aceptada
- **Decisión:** en Linux, paquetes del sistema (`apt`) para desarrollar; en Windows y macOS, **vcpkg en modo manifiesto** (`vcpkg.json`, se añadirá cuando entre la primera biblioteca externa, en la Fase 1 con SQLite). Para los paquetes de Linux (.deb/AppImage) las bibliotecas se empaquetan junto a la aplicación.
- **Alternativas:** vcpkg también en Linux (compilaciones más lentas, sin beneficio al desarrollar); Conan (otra herramienta más que aprender).
- **Consecuencias:** las versiones pueden diferir entre Linux y Windows/macOS; `docs/DEPENDENCIES.md` fija la versión mínima admitida de cada una.

## ADR-0004 — Pruebas con Qt Test
- **Fecha:** 2026-11-11 · **Estado:** aceptada
- **Decisión:** Qt Test + CTest. Ya viene con Qt (sin dependencia nueva), entiende los tipos de Qt en los mensajes de error, y trae `QSignalSpy`, `QTemporaryDir` y `QBENCHMARK`.
- **Alternativas:** GoogleTest (mejor para simulacros, pero es otra dependencia y no conoce `QString`).

## ADR-0005 — Listado de carpetas con API nativa por plataforma
- **Fecha:** 2026-11-11 · **Estado:** aceptada
- **Contexto:** medido en este equipo con 100 000 archivos (ver `BENCHMARKS.md`): `readdir`+`fstatat` 0,20 s; `std::filesystem` 0,44 s; `QDirIterator` 0,76 s; `QDir::entryInfoList` 0,79 s y además no entrega nada hasta los 90 ms.
- **Decisión:** el módulo `fs` define una interfaz de listado progresivo y la implementa con la API nativa de cada sistema: `readdir`/`statx` en Linux, `FindFirstFileExW` con `FIND_FIRST_EX_LARGE_FETCH` en Windows (entrega tamaño y fechas sin llamada extra) y `getattrlistbulk` en macOS. `std::filesystem` queda como implementación de respaldo y de referencia en pruebas.
- **Consecuencias:** tres implementaciones que mantener, cubiertas por las mismas pruebas. Las de Windows y macOS no están medidas todavía.

## ADR-0006 — Ajuste de las metas de rendimiento
- **Fecha:** 2026-11-11 · **Estado:** aceptada (provisional)
- **Contexto:** §4 pide medir una línea base y ajustar. El equipo de desarrollo (Ryzen 7 2700X, 60 GB) es más potente que el de referencia, así que las metas de §4 **se mantienen** para el equipo de referencia y se añaden metas más exigentes para este equipo, donde sí se puede medir.
- **Decisión (equipo de desarrollo, SSD, caché caliente):**
  | Escenario | Meta §4 | Línea base medida | Meta en este equipo |
  |---|---|---|---|
  | Listar 100 000 archivos, completo | < 3 s | nativo `gio` 0,65 s; `readdir` 0,20 s | **< 1 s** con orden incluido |
  | Primeros elementos visibles | < 300 ms | 200 elementos leídos en ~1 ms | **< 100 ms** |
  | Filtrado local, 100 000 elementos | < 100 ms | 17 ms (un hilo, sin optimizar) | **< 50 ms** |
  | Orden natural, 100 000 nombres | — | 247 ms (un hilo) | en segundo plano; nunca en el hilo de interfaz |
  | Copia grande sin verificación | ≥ 90 % de la nativa | `cp` 373–418 MB/s; bucle propio 405–443 MB/s | se mantiene ≥ 90 % |
- **Pendiente:** medir con caché fría, en HDD y USB, en Windows (VM) y con recursos limitados a 4 núcleos/8 GB para aproximar el equipo de referencia. Bloqueo de interfaz, memoria e índice se medirán cuando exista el código (Fases 1 y 2).

## ADR-0007 — PDF con Qt PDF
- **Fecha:** 2026-11-11 · **Estado:** aceptada
- **Decisión:** Qt PDF (módulo oficial, LGPL-3/GPL-3, basado en PDFium) para miniaturas, previsualización, texto e impresión de PDF.
- **Alternativas:** Poppler (GPL, compatible, pero su versión Qt6 va atada al Qt del sistema y es laboriosa de compilar en Windows/macOS).

## ADR-0008 — El equipo de desarrollo usa Ubuntu 26.04
- **Fecha:** 2026-11-11 · **Estado:** aceptada
- **Contexto:** §1 fija Ubuntu 22.04 y 24.04 como mínimos, pero este equipo se actualizó a 26.04 el 4-nov-2026.
- **Decisión:** desarrollo y pruebas manuales en 26.04; 22.04 y 24.04 se compilan y prueban en la CI, y los paquetes se generan en 22.04 (la glibc más antigua) para que funcionen en las tres. Se propone añadir 26.04 a la lista oficial (ver informe de la Fase 0).
- **Consecuencias:** las pruebas manuales de interfaz en 22.04/24.04 necesitarían una VM de Ubuntu; se creará si el usuario lo pide.

## ADR-0009 — VM de Windows dedicada al proyecto
- **Fecha:** 2026-11-11 · **Estado:** aceptada
- **Contexto:** ya existía una VM personal del usuario en el equipo.
- **Decisión:** crear una VM aparte, `EU-Win11-Build` (Windows 11 Enterprise de evaluación 26H2 en español, 4 CPU, 8 GB, disco de 100 GB dinámico en `~/VMs`), para no alterar la del usuario y poder usar instantáneas y reinstalar sin riesgo.
- **Consecuencias:** la evaluación dura 90 días (hasta ~9-feb-2027); después se reinstala con el mismo procedimiento o se reactiva con `slmgr /rearm`. Windows 10 22H2 no se prueba en VM por ahora: Microsoft dejó de darle soporte en octubre de 2025 (ver informe, sección B).

## ADR-0010 — Aprobación de la Fase 1 y repositorio público
- **Fecha:** 2026-11-11 · **Estado:** aceptada (instrucción del usuario)
- **Decisión:** el usuario aprobó iniciar la Fase 1. El repositorio se publica en GitHub como `explorador-ultra` (público, para disponer de runners de macOS). Los commits usan el correo `noreply` de la cuenta de GitHub.
- **Consecuencias:** desde este punto se aplica la regla 15 de §25: ramas por fase y fusión a `main` solo con la CI en verde.

## ADR-0011 — SQLite a través de Qt SQL
- **Fecha:** 2026-11-11 · **Estado:** aceptada (sustituye la parte de SQLite de ADR-0003)
- **Contexto:** el historial, las etiquetas y el índice necesitan SQLite. Los binarios oficiales de Qt 6.12 ya traen el controlador `QSQLITE` con SQLite 3.53.4 incluido, compilado con FTS5 y seguro entre hilos (comprobado en Linux).
- **Decisión:** usar Qt SQL (`QSqlDatabase`, controlador `QSQLITE`) en lugar de enlazar SQLite por separado. Cada hilo abre su propia conexión; las bases van en modo WAL.
- **Alternativas:** SQLite del sistema en Linux y vcpkg en Windows/macOS (versiones distintas por plataforma y una dependencia más que empaquetar).
- **Consecuencias:** la misma versión de SQLite en las tres plataformas y sin vcpkg por ahora. El módulo `journal` depende de `Qt6::Sql`. Si en la Fase 2 el índice necesitara funciones de SQLite que Qt SQL no expone, se reevaluará.

## ADR-0012 — Nombre y estructura de la papelera
- **Fecha:** 2026-11-11 · **Estado:** aceptada (precisa SPEC §10)
- **Contexto:** §10 pide una carpeta `PapeleraExploradorUltra` por volumen, oculta "según la convención de cada plataforma". En Linux y macOS, oculto significa nombre con punto inicial; en Windows es un atributo. Un mismo disco externo puede usarse en los tres sistemas.
- **Decisión:** la carpeta se llama `.PapeleraExploradorUltra` en todas las plataformas y en Windows recibe además el atributo de oculto. Dentro, cada elemento ocupa `<id>/<nombre original>` y `<id>.info` (JSON con ruta original, fecha, tamaño e id). El `.info` se escribe antes de mover el elemento.
- **Alternativas:** nombre sin punto y archivo `.hidden` (solo lo respetan algunos exploradores de Linux).
- **Consecuencias:** en el disco del sistema de Linux la papelera queda en `~/.PapeleraExploradorUltra` (la raíz no es escribible). Los `.info` cuyo id no coincide con su nombre de archivo se ignoran, para que un archivo manipulado no pueda apuntar fuera de la papelera.
