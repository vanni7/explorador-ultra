# Estado del proyecto

Última actualización: 2026-11-15 · Fase actual: **3 (en curso)**; la Fase 2 se entregó el 14-nov con puntos abiertos (ver `INFORME_FASE_2.md`)

Estados: **Terminada** (cumple §26) · **Parcial** · **Pendiente** · **Bloqueada**.

## Punto de reanudación (se actualiza tras cada paso; si hubo un corte, empieza aquí)
Fase 3 — Miniaturas, previsualización y metadatos, rama `fase-3/miniaturas` (sale de `main` con la Fase 2 ya fusionada). Cada paso terminado queda en un commit ya enviado a GitHub; lo que no esté en `git log` no está hecho. Antes de seguir: `git status` y compilar con el preset `release`. Si git dice «objeto vacío» tras un corte, ver las notas de `CLAUDE.md`.

| # | Paso | Estado |
|---|---|---|
| 1 | Módulo `thumbs`: miniaturas de imágenes (JPEG, PNG, GIF, WebP, TIFF, SVG… con orientación EXIF, sin agrandar, tope contra imágenes absurdas), caché en memoria (256 MB) y en disco (`CacheLocation/miniaturas`, clave ruta + tamaño + fecha + lado, recuerda los fallos), servicio en segundo plano con cancelación de lo que deja de verse | **Hecho** (15-nov): 11 pruebas en `test_thumbs` (Ubuntu release). Falta: CI |
| 2 | Vista de miniaturas en la pestaña (cambiar entre detallada y miniaturas, tamaño ajustable, carga solo de lo visible) | **Hecho** (15-nov): `ThumbnailView` comparte modelo y selección con la detallada; Ver → Detalles (Ctrl+1) / Miniaturas (Ctrl+2), tres tamaños, botones en la barra; renombrar, arrastrar, soltar y menú contextual. Prueba `thumbnailViewKeepsTheSelectionAndShowsImages`; visto con ratón simulado en `~/Imágenes` (fotos reales, la vertical sale vertical). Falta: CI; medir con miles de imágenes; ordenar desde esta vista (no hay cabecera) |
| 3 | Carpetas (hasta cuatro miniaturas o composición), PDF (Qt PDF), texto/código/Markdown (extracto) | **Hecho** (15-nov): PDF con el complemento de imagen de Qt PDF (primera página, fondo blanco; sin el complemento no hay miniatura); texto, código y Markdown como una hoja con sus primeras líneas (UTF-8, UTF-16, Latin-1; los binarios no); carpetas con hasta cuatro miniaturas si los primeros archivos son de la misma clase. 14 pruebas en `test_thumbs`. **Falta**: carpetas con contenido mezclado (la especificación pide una composición de iconos; ahora se ve el icono de carpeta) y carpeta vacía con icono propio; Markdown se ve como texto plano, sin renderizar; CI con el complemento de PDF |
| 4 | Video (fotograma del segundo 3) y audio (carátula, duración), con límite de decodificadores | **Hecho** (15-nov) con FFmpeg 6.1+ (opcional; en Ubuntu 22.04, con FFmpeg 4.4, no hay miniaturas de video): fotograma del segundo 3 o de la mitad, proporción de píxel, giro del móvil, duración en una esquina; audio con carátula incrustada o genérica y duración. El límite de decodificadores a la vez es el de hilos de miniaturas (2–6 según núcleos). Pruebas con video y FLAC generados en la prueba; visto en tus carpetas `SOLO_PELIS/VIDEOS` y de MP3. Corregido de paso: con una carpeta como argumento no se leían las preferencias. **Falta**: CI con FFmpeg en macOS y Windows; aislar la decodificación (paso 7) |
| 5 | RAW (LibRaw) y ofimática (miniatura incrustada; LibreOffice en proceso aparte con tiempo límite) | **Hecho** (15-nov): ODF/OOXML con la miniatura que llevan dentro (libarchive, opcional); si no la llevan, y para DOC/XLS/PPT/RTF, LibreOffice en proceso aparte con perfil propio, uno a la vez, 30 s como máximo y cancelable; RAW con la vista previa de la cámara (LibRaw `libraw_r`, opcional) y su giro. Documentos y videos van con prioridad más baja que las fotos. 19 pruebas en `test_thumbs` (LibreOffice pintó un DOCX en 0,7–1,1 s). **Falta**: probar RAW con fotos reales (no hay en el equipo); CI sin LibreOffice (esa prueba se salta) |
| 6 | Panel de previsualización y metadatos (EXIF, ID3, códec, resolución, duración) | **Hecho** (15-nov): módulo `meta` (dimensiones; EXIF con Exiv2 opcional; duración, códecs, resolución, fotogramas, canales y etiquetas con FFmpeg; páginas de PDF) y `PreviewPanel` a la derecha (Alt+P, Ver): datos comunes y del tipo arriba; abajo imagen, primera página, fotograma, carátula, texto (512 kB) o Markdown formateado (sin abrir enlaces). Carga en segundo plano, cancela al cambiar de selección, funciona en resultados de búsqueda. Pruebas `test_meta` (7) y `previewPanelShowsDataAndContent`; visto con tus fotos (EXIF real). **Falta**: resaltado de código; recorrer páginas del PDF; reproducir audio y video (paso 8); CI |
| 6b | Fallos encontrados al probar en Windows (15-nov) | **Corregido**: (1) el servicio de miniaturas daba a sus tareas un puntero a la caché sin esperarlas; al cerrar la ventana con una miniatura en marcha se escribía en memoria liberada (lo más probable detrás del cierre sin salida de `test_ui_main_window` en el Windows de la CI); ahora la caché se comparte. (2) Pruebas del planificador y de miniaturas que, si fallaban a medias, dejaban tareas usando variables ya destruidas (cierre 0xc0000409 en la VM, donde dormir 1 ms dura ~15 ms) |
| 7 | Decodificación de riesgo en un proceso auxiliar (archivos dañados o maliciosos no cierran la aplicación) | **Hecho** (15-nov): `eu-thumbnailer`, uno por hilo de miniaturas, reutilizado; si un archivo lo cierra o lo cuelga (30 s) se marca como fallido y se arranca otro; límite de 3 GB (`RLIMIT_DATA`) en Unix; sin el auxiliar se decodifica en el propio proceso. Lo usan las miniaturas y la vista previa. Coste medido: +9 % con fotos, +24 % con PDF. CI verde en las seis plataformas (`8a2eaf4`), tras corregir que con ASan el límite de memoria lo cerraba al arrancar. Pruebas que lo cierran y lo cuelgan a propósito. **Falta**: es aislamiento frente a cierres, no un entorno restringido (corre con los permisos del usuario); en Windows no hay límite de memoria; los metadatos (`meta`) aún se leen en el propio proceso |
| 8 | Reproducción en miniatura de video y audio | Siguiente |
| 9 | CI con Qt PDF/Multimedia en las tres plataformas, mediciones, verificación manual, fusionar a `main`, informe | Pendiente |

## Pasos de la Fase 2 (entregada el 14-nov; ver `INFORME_FASE_2.md`)
Rama `fase-2/busqueda`, fusionada a `main`.

| # | Paso | Estado |
|---|---|---|
| 1 | Módulo `search`: gramática, analizador y evaluador de consultas; `docs/SEARCH_SYNTAX.md` | **Hecho** (48 pruebas en `test_search_query`, Ubuntu; 100 000 nombres en ~15 ms). `tag:` y `content:` se aceptan pero se informan como no disponibles. Falta ver la CI |
| 2 | Filtrado local de la carpeta a la vista (meta: 100 000 elementos < 50 ms por tecla) | **Hecho**: barra «Filtrar esta carpeta» (Ctrl+I, menú Ver), 5–7 ms sobre 100 000 elementos. Pruebas en Ubuntu release y ASan. Falta: que el dueño lo pruebe, medir el repintado, CI |
| 2b | Ubicación «Unidades» (pedida por el dueño el 12-nov): clic en el rótulo «Unidades» del árbol, o subir desde la raíz de un disco, muestra las unidades con tamaño total, ocupado, libre y barra de uso | **Hecho**: `VolumesView`, `Locations.h`; pruebas en Ubuntu release y ASan, vista con ratón simulado en X11, CI verde. Falta: que el dueño lo pruebe y usarla a mano en Windows |
| 3 | Búsqueda recursiva progresiva (pausa, cancelación < 500 ms, contadores) y resultados como ubicación virtual | **Hecho**: motor `search::searchTree` (pausa, cancelación, contadores, tope de 200 000 resultados, no sigue enlaces ni cruza a otras unidades) y la interfaz: buscador de la barra (Ctrl+F, Intro), resultados como ubicación `buscar:` de la pestaña (Atrás/Subir vuelven), franja con avance, Pausar/Reanudar/Detener/Buscar de nuevo, orden en segundo plano, menú contextual con «Abrir la carpeta que lo contiene», copiar/cortar/papelera sobre lo encontrado. Pruebas en Ubuntu release, ASan y TSan (motor); búsqueda real en la carpeta personal con ratón simulado (X11). CI verde en las seis plataformas (commit `tests: la búsqueda en Windows…`). Falta: que el dueño lo pruebe, usar la ventana a mano en Windows, renombrar desde resultados, soltar sobre resultados |
| 3b | Ajustes pedidos por el dueño el 13-nov tras probarlo | **Hecho**: (1) en el árbol, el clic ya no pasa el teclado a la lista y las flechas actúan sobre el árbol (Izquierda recoge/sube al padre, Derecha despliega/baja, Arriba/Abajo recorren y la lista sigue tras 300 ms); (2) `walter riso*` ya encuentra «Walter Riso…» (varias palabras con comodines valen también como frase); (3) opción «Distinguir mayúsculas y minúsculas»; (4) búsqueda en el contenido de archivos de texto (`content:` y opción «El contenido»), sin PDF ni ofimática hasta la Fase 4. Pruebas en Ubuntu release, ASan y TSan; CI verde en las seis plataformas (`8570dfb`); caso real del dueño comprobado en X11 simulado: `walter riso*` en `/media/merwin/RESPALDO` da 102 resultados tras revisar 135 469 carpetas (unos segundos, sin cronometrar) |
| 3c | Pedido del dueño el 13-nov: que no se borre lo buscado al cambiar de carpeta | **Hecho**: el texto del buscador se conserva al navegar; flecha del buscador o tecla Abajo listan las 15 últimas búsquedas (se guardan al cerrar, se pueden vaciar). Prueba en `searchScopesAndOptions`; visto en X11 simulado |
| 4 | Alcances: carpeta, con subcarpetas, unidad, varias unidades, todas | **Hecho**: botón «Opciones» junto al buscador (`SearchScope`, `SearchSpec` en `Locations.h`); desde «Unidades» se busca en todas. Prueba `searchScopesAndOptions` con unidades simuladas. Falta: que el dueño lo pruebe; no se ha medido una búsqueda en todas las unidades reales; el diálogo de elegir unidades solo se ha visto en X11 simulado |
| 5 | Índice SQLite opcional, vigilancia de cambios, asistente de primer arranque, resultados «del índice» | **Hecho** (13-nov). Módulo `index`: `Index` (SQLite, sincronización por diferencias, búsqueda con la misma `Query`), `TreeWatcher` (inotify con tope del 60 % del límite, ReadDirectoryChangesW, FSEvents), `IndexService` (segundo plano, una tarea a la vez, pausa, puesta al día al arrancar, repaso cada 30 min de lo no vigilado, conserva el índice si la unidad no está). Interfaz: asistente de primer arranque, Ver → «Índice de búsqueda…», columna «Origen», «Verificar en disco», avance en la barra de estado. Pruebas: `test_index`, `test_index_tree_watcher`, `test_index_service`, `searchUsesTheIndexWhenThereIsOne`; 21 en release y ASan, 19 en TSan; CI verde en las seis plataformas (`216070a`). Visto con ratón simulado (X11) indexando `/media/merwin/RESPALDO`. Mediciones en `BENCHMARKS.md`. **Falta**: que el dueño lo pruebe; medir con 1 000 000 de entradas (lo mayor medido son 573 000), caché fría y HDD; usar las ventanas a mano en Windows y macOS (allí solo corren las pruebas); la búsqueda en varias unidades solo usa el índice si todas lo tienen; fanotify y el servicio MFT de Windows no están hechos (Fase 5) |
| 6 | Concurrencia adaptativa por dispositivo en `tasks` | **Hecho** (13-nov): `Scheduler::submit` con dispositivos y `submitForPaths`, límites por clase de unidad sumando todas las categorías, tres niveles de urgencia, cancelar una tarea en espera la suelta al instante; `fs::DeviceMap`; listados, tamaños de carpeta, búsquedas, indexación, copiar y mover declaran su unidad; límites en `[concurrencia]` del archivo de configuración. Pruebas: 19 en `test_tasks_scheduler`, `test_fs_device_map`; 22 programas en release. **Falta**: no se ha medido el efecto en un disco mecánico real (copias y listados a la vez); no hay pantalla de ajustes; una tarea de fondo ya empezada no cede el disco a una copia que llega después |
| 6b | Ceder el disco a lo interactivo (15-nov, al medir en los discos reales) | **Hecho**: `CancelToken::yieldToInteractive()`; lo usan copia, tamaño de carpetas e indexación. Medido en el disco mecánico YEILY (11 s → 0,3 s) y en el SSD del sistema (4 s → 0,22 s). Pruebas en `test_tasks_scheduler`. **Falta**: escrituras, USB y red; la búsqueda (interactiva) no cede, así que una búsqueda larga en un disco frena las copias en él |
| 7 | Mediciones (`BENCHMARKS.md`), verificación manual, CI, fusionar a `main`, informe | **Hecho** (14-nov): índice medido con 1 000 000 de entradas; 22 pruebas en release, ASan, VM de Windows y CI; informe en `INFORME_FASE_2.md`. **Abierto**: `test_ui_main_window` falló una vez de tres en Windows de la CI tras el paso 6, sin dejar salida; pasó al relanzar y cinco veces seguidas en la VM. Causa sin encontrar; la CI ahora repite con detalle lo que falle |

Pasos de la Fase 1 (sesión del 12-nov): todos hechos; resumen en `INFORME_FASE_1.md`.

## Fase 0 — Arquitectura y entorno
| Tarea | Estado | Verificado en | Qué falta |
|---|---|---|---|
| Entorno en Ubuntu (compiladores, CMake, Qt 6.12, bibliotecas) | Terminada | Ubuntu 26.04 | — |
| "Hola mundo" Qt: compila, pasa pruebas y arranca | Parcial | Ubuntu 26.04 (Wayland y XWayland; ASan+UBSan) y Windows 11 en la VM (MSVC; arranque con plataforma `offscreen`) | Ver la ventana en el escritorio de Windows; macOS (CI) |
| VM de Windows `EU-Win11-Build` | Terminada | Compila y pasa las pruebas dentro de la VM | Falta Qt PDF (Fase 3) |
| Repositorio Git local | Terminada | — | — |
| Repositorio en GitHub y CI de las tres plataformas | Terminada | CI en verde en Ubuntu 22.04/24.04, Windows, macOS Intel y ARM | — |
| ADR Widgets frente a Quick | Terminada | — | ADR-0002 |
| Arquitectura, estructura de carpetas, dependencias y licencias | Terminada | — | — |
| Icono (SVG, PNG, .ico, .icns) | Terminada | Vista a 16 y 256 px | — |
| Mockup de la interfaz | Terminada | — | `docs/mockup/` |
| Herramienta de medición `eu-bench` y línea base | Parcial | Ubuntu 26.04, SSD | HDD, USB, caché fría, Windows, equipo de referencia |
| Ajuste de metas (ADR-0006) | Parcial | — | Provisional hasta completar las mediciones |
| Plan de pruebas | Terminada | — | `docs/TEST_PLAN.md` |
| Riesgos por sistema operativo | Terminada | — | `docs/RISKS.md` |

## Código existente
| Módulo | Función | Estado | Notas |
|---|---|---|---|
| `core` | Versión y nombre de la aplicación | Parcial | Probado en Linux y en la VM de Windows; falta CI |
| `core` | Orden natural (`naturalCompare`) | Parcial | 6 pruebas en verde en Linux y en la VM de Windows; falta CI (macOS sin probar) |
| `app` | Ventana mínima con `--smoke-test` | Parcial | No es todavía el explorador |
| `bench` | `eu-bench` (mktree, listing, sort, copy) | Parcial | `copy` solo en Linux/macOS |

## Fase 1 — Explorador funcional (entregada con puntos abiertos)
Rama `fase-1/core`. "CI completa" = Ubuntu 22.04/24.04, ASan, Windows, macOS Intel y ARM.

| Función | Estado | Verificado en | Qué falta |
|---|---|---|---|
| `core`: errores tipados, tamaños SI/binarios, validación de nombres, registro rotativo | Parcial | CI completa | Verificación manual |
| `tasks`: planificador por categorías con prioridades y cancelación | Parcial | CI completa + TSan local | Concurrencia adaptativa por dispositivo (Fase 2) |
| `fs`: listado nativo por lotes | Parcial | CI completa | Medir en Windows y macOS; `getattrlistbulk` en macOS; nombres con bytes no UTF-8 en Linux |
| `fs`: unidades y conexión en caliente | Parcial | Ubuntu (7 unidades reales con su tipo) | Probar conectar/desconectar una USB a mano; tipo SSD/HDD en Windows y macOS |
| `fs`: vigilancia de la carpeta visible | Parcial | CI completa | — |
| Ventana, pestañas básicas y sesión | Parcial | Ubuntu (captura), VM de Windows (pruebas), CI de Linux y macOS | Recuperar pestañas cerradas, fijar y bloquear (Fase 5) |
| Árbol de lugares (favoritos y unidades) | Parcial | Ubuntu (captura) | Unidades de red con tiempo de espera; favoritos editables |
| Navegación e historial, barra de ruta con migas | Parcial | Ubuntu + pruebas | Verificación manual con teclado y ratón |
| Vista detallada virtualizada con orden natural | Parcial | Ubuntu: 100 000 archivos en ~365 ms, primeras filas a 21 ms | Medir el bloqueo del hilo de interfaz; columnas opcionales |
| `journal`: historial persistente (write-ahead) | Parcial | CI completa; VM de Windows | Retención configurable (hoy fija: 30 días o 10 000 operaciones, aplicada al arrancar) |
| `ops`: operaciones elementales (copia atómica, renombrar sin sobrescribir, borrado sin seguir enlaces) | Parcial | CI completa; VM de Windows con zona horaria de México | Verificación manual. El 12-nov se corrigió que la copia desplazaba la fecha de modificación en Windows para fechas de antiguos horarios de verano |
| `trash`: papelera propia por volumen | Parcial | CI completa; Ubuntu: raíz de volumen (tmpfs) y carpeta personal (disco del sistema) | Retención y tamaño máximo; probar en una USB real |
| Motor de operaciones (`ops::Engine`): crear, renombrar, copiar, mover (también entre unidades), papelera, restaurar, eliminar, deshacer | Parcial | CI completa; Ubuntu (17 pruebas, ASan, TSan), con movimiento real entre tmpfs y ext4 | Sobrescribir y demás políticas de colisión (Fase 4); deshacer una restauración |
| Recuperación tras cierre inesperado | Parcial | Ubuntu: proceso matado a mitad de mover y de copiar; diálogo al arrancar con revertir, descartar o decidir más tarde | "Reanudar" no existe (botón deshabilitado); ver el diálogo a mano |
| Operaciones en la interfaz (menú Edición y contextual, atajos, renombrado en línea, portapapeles, arrastrar y soltar, progreso con Cancelar, avisos de error) | Parcial | Ubuntu: pruebas automáticas (release, ASan) | **Nadie lo ha usado aún con teclado y ratón**; arrastrar de verdad en Wayland/X11/Windows y con otras aplicaciones; marcar visualmente lo cortado; «Abrir con…» y «Propiedades» no existen |
| Papelera: ventana unificada (restaurar, eliminar, vaciar) | Parcial | Ubuntu: pruebas automáticas | Verla a mano; retención y tamaño máximo; unidades desconectadas como «no disponibles» |
| Tamaño de carpetas (opción del menú Ver) | Parcial | Ubuntu: pruebas automáticas | Ordenar por ese tamaño; medir en disco mecánico; verlo a mano en una carpeta grande |
| Atajos principales | Parcial | Pruebas de las acciones (no de las teclas) | F2, Supr, Mayús+Supr, Ctrl+C/X/V, Ctrl+Z, Ctrl+A, Ctrl+Mayús+N: comprobar cada tecla a mano; los de macOS no se han probado en un Mac |
| Manejo de errores visible | Parcial | Pruebas | Carpeta ilegible: mensaje y "Reintentar". Operaciones: aviso que no bloquea, con el detalle de cada error |
| Búsqueda en subcarpetas | Parcial (Fase 2, paso 3) | Automáticas en Ubuntu; a mano solo con ratón simulado | Sin índice; el alcance es la carpeta de la pestaña con sus subcarpetas, dentro de la misma unidad. Faltan alcances (paso 4) e índice (paso 5) |

### Dónde quedó
Ver la tabla «Punto de reanudación» al principio de este archivo: se actualiza tras cada paso.

## Fases 2 a 6
Todas **Pendientes**. Plan y criterios en `docs/PHASE_PLAN.md`.
