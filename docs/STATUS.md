# Estado del proyecto

Última actualización: 2026-11-12 · Fase actual: **2 (por empezar)**; la Fase 1 se entregó el 12-nov con puntos abiertos (ver `INFORME_FASE_1.md`)

Estados: **Terminada** (cumple §26) · **Parcial** · **Pendiente** · **Bloqueada**.

## Punto de reanudación (se actualiza tras cada paso; si hubo un corte, empieza aquí)
Fase 2 — Búsqueda y rendimiento, rama `fase-2/busqueda` (sale de `main` con la Fase 1 ya fusionada). Cada paso terminado queda en un commit ya enviado a GitHub; lo que no esté en `git log` no está hecho. Antes de seguir: `git status` y compilar con el preset `release`. Si git dice «objeto vacío» tras un corte, ver las notas de `CLAUDE.md`.

| # | Paso | Estado |
|---|---|---|
| 1 | Módulo `search`: gramática, analizador y evaluador de consultas; `docs/SEARCH_SYNTAX.md` | **Hecho** (48 pruebas en `test_search_query`, Ubuntu; 100 000 nombres en ~15 ms). `tag:` y `content:` se aceptan pero se informan como no disponibles. Falta ver la CI |
| 2 | Filtrado local de la carpeta a la vista (meta: 100 000 elementos < 50 ms por tecla) | **Hecho**: barra «Filtrar esta carpeta» (Ctrl+I, menú Ver), 5–7 ms sobre 100 000 elementos. Pruebas en Ubuntu release y ASan. Falta: que el dueño lo pruebe, medir el repintado, CI |
| 3 | Búsqueda recursiva progresiva (pausa, cancelación < 500 ms, contadores) y resultados como ubicación virtual | Siguiente |
| 4 | Alcances: carpeta, con subcarpetas, unidad, varias unidades, todas | Pendiente |
| 5 | Índice SQLite opcional, vigilancia de cambios, asistente de primer arranque, resultados «del índice» | Pendiente |
| 6 | Concurrencia adaptativa por dispositivo en `tasks` | Pendiente |
| 7 | Mediciones (`BENCHMARKS.md`), verificación manual, CI, fusionar a `main`, informe | Pendiente |

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
| Búsqueda | Pendiente (Fase 2) | — | El campo aparece deshabilitado con el rótulo "próximamente" |

### Dónde quedó
Ver la tabla «Punto de reanudación» al principio de este archivo: se actualiza tras cada paso.

## Fases 2 a 6
Todas **Pendientes**. Plan y criterios en `docs/PHASE_PLAN.md`.
