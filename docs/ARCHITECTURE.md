# Arquitectura

## Estructura de carpetas
```
Explorador-Ultra/
├── CMakeLists.txt, CMakePresets.json   construcción (presets: debug, release, asan, tsan)
├── cmake/                              opciones de compilación comunes
├── src/<módulo>/                       un módulo = una biblioteca
│   ├── include/eu/<módulo>/            cabeceras públicas (interfaz mínima)
│   └── src/                            implementación privada
├── src/app/                            ejecutable
├── tests/<módulo>/                     pruebas de cada módulo (Qt Test)
├── bench/                              eu-bench: mediciones reproducibles
├── assets/icon/                        icono maestro SVG y derivados
├── packaging/{linux,windows,macos}/    (Fase 6) .deb, AppImage, instalador, .dmg
├── translations/                       (Fase 1) archivos .ts de Qt Linguist
├── tools/                              guiones auxiliares
├── docs/                               documentación
└── .github/workflows/                  CI
```
Hoy existen `core`, `app`, `tests/core` y `bench`. Los demás módulos se crean en la fase que los necesita.

## Capas y dependencias entre módulos
Un módulo solo puede depender de módulos de capas inferiores. Solo la capa 5 usa Qt Widgets/Gui.

| Capa | Módulos | Depende de |
|---|---|---|
| 0 | `core` | Qt Core |
| 1 | `tasks`, `settings` | `core` |
| 2 | `fs`, `journal`, `tags` | capa 1, SQLite |
| 3 | `trash`, `ops`, `meta`, `archive`, `index` | capa 2 (+ libarchive, Exiv2, TagLib) |
| 4 | `search`, `thumbs`, `preview`, `rename`, `organize`, `dupes`, `similar`, `sync`, `print` | capa 3 (+ FFmpeg, LibRaw, Qt PDF, xxHash) |
| 5 | `ui`, `app` | todo lo anterior, Qt Widgets |
| aparte | `indexsvc-win` | solo `core`; proceso separado, habla con `index` por *named pipe* |

Nota de implementación (Fase 2): `index` depende de `search`, y no al revés como dice la tabla. El índice evalúa la misma `search::Query` que la búsqueda directa y entrega los mismos `search::Hit`, de modo que una consulta da lo mismo por los dos caminos; quien elige entre índice y disco es la interfaz (`BrowserPane`). `index` contiene `Index` (la base SQLite de una unidad: sincronización por diferencias y búsqueda), `TreeWatcher` (inotify, ReadDirectoryChangesW o FSEvents) e `IndexService` (qué unidades se indexan y el trabajo en segundo plano).

Nota de implementación: `ops` contiene las operaciones elementales (`FileOps`) y el motor (`Engine`). Como el motor usa la papelera y la papelera usa las operaciones elementales, el motor se compila como biblioteca aparte (`eu_ops_engine`), después de `eu_trash`.

Relaciones clave: `ops` escribe en `journal` **antes** de tocar el disco; `trash`, `rename`, `organize`, `sync` y `dupes` nunca modifican archivos por su cuenta: piden las operaciones a `ops`, de modo que todo queda en el historial y se puede deshacer. `thumbs`, `preview` y `print` usan `QImage` (Qt Gui) pero no Widgets.

## Ubicación virtual (`Location`)
`eu::fs::Location` identifica un lugar navegable con un esquema y una ruta:

| Esquema | Ejemplo | Proveedor |
|---|---|---|
| `file` | `file:///home/ana/Fotos` | `fs` (API nativa) |
| `archive` | `archive:///home/ana/a.zip!/docs` | `archive` |
| `search` | `search://<id de consulta>` | `search` |
| `trash` | `trash://` (todos los volúmenes) | `trash` |

Cada proveedor implementa la misma interfaz `LocationProvider`: listar de forma progresiva, obtener datos de un elemento, abrir para lectura, vigilar cambios y declarar sus capacidades (¿se puede escribir?, ¿renombrar?, ¿tiene miniaturas?). La interfaz, la previsualización y las operaciones solo conocen esa interfaz. Las unidades de red son `file` con capacidades distintas (tiempos de espera, sin papelera en la raíz).

## Modelo de hilos
```
Hilo de interfaz ── solo pinta y atiende eventos; nunca E/S
      │ pide trabajo                       ▲ resultados por lotes (señales en cola)
      ▼                                    │
 tasks::Scheduler ── pools acotados por categoría:
   listado · transferencias · hashing · multimedia · miniaturas · indexación · metadatos · compresión
      │
      └─ limitador por dispositivo (SSD: varios flujos; HDD: 1 secuencial; USB y red: conservador)
```
- Cada tarea lleva una prioridad (las seis de §3.3), una señal de cancelación cooperativa (`std::stop_token`) y un canal de progreso.
- Los resultados vuelven al hilo de interfaz **por lotes** (p. ej. cada 50 ms o 500 elementos) para no inundarlo: así se cumplen "primeros elementos < 100 ms" y "bloqueo < 50 ms".
- Toda excepción se captura en el límite de la tarea y se convierte en un `eu::core::Error` que viaja con el resultado (regla 6 de §25).
- El tipo de dispositivo se detecta por volumen (`/sys/block/*/queue/rotational` en Linux, `IOCTL_STORAGE_QUERY_PROPERTY` en Windows, IOKit en macOS).

## Flujo de datos: abrir una carpeta
1. `ui` pide a `fs` listar una `Location` → se encola una tarea de prioridad "navegación visible".
2. El listador nativo lee entradas y las entrega en lotes; el modelo de la vista las inserta y la vista pinta solo lo visible.
3. El orden natural se calcula en segundo plano y se aplica al terminar (medido: 247 ms para 100 000 nombres, demasiado para el hilo de interfaz).
4. `fs` instala un vigilante (inotify / ReadDirectoryChangesW / FSEvents) y manda cambios incrementales al modelo.
5. Las tareas secundarias (tamaño de carpetas, miniaturas, etiquetas) se piden solo para lo visible y se cancelan al salir.

## Persistencia
- Preferencias: archivo legible (`QSettings` en formato INI) en `QStandardPaths::AppConfigLocation`.
- Bases SQLite en modo WAL en `AppDataLocation`: `journal.db`, `tags.db`, `index.db` (una por volumen), `thumbs.db` + archivos de miniaturas en `CacheLocation`.
- Cada base guarda su número de esquema (`PRAGMA user_version`) y se actualiza con migraciones numeradas.
