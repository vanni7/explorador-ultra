# Estado del proyecto

Última actualización: 2026-11-11 · Fase actual: **1 (en curso)**; la Fase 0 fue aprobada

Estados: **Terminada** (cumple §26) · **Parcial** · **Pendiente** · **Bloqueada**.

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

## Fase 1 — Explorador funcional (en curso)
Rama `fase-1/core`. Verificado = pruebas automáticas en la CI (Ubuntu 22.04/24.04, Windows, macOS Intel y ARM) salvo que se indique.

| Función | Estado | Verificado en | Qué falta |
|---|---|---|---|
| `core`: errores tipados, tamaños SI/binarios, validación de nombres, registro rotativo | Parcial | CI completa | Verificación manual; uso real desde `ops` |
| `tasks`: planificador por categorías con prioridades y cancelación | Parcial | CI completa + TSan local | Concurrencia adaptativa por dispositivo (Fase 2) |
| `fs`: listado nativo por lotes | Parcial | CI completa | Medir en Windows y macOS; `getattrlistbulk` en macOS; nombres con bytes no UTF-8 en Linux |
| `fs`: unidades y conexión en caliente | Parcial | Ubuntu (7 unidades reales detectadas con su tipo) | Probar conectar/desconectar una USB a mano; tipo SSD/HDD en Windows y macOS |
| `fs`: vigilancia de la carpeta visible | Parcial | CI completa | — |
| Ventana, pestañas básicas y sesión | Parcial | Ubuntu (captura) + pruebas en CI pendientes de este envío | Recuperar pestañas cerradas, fijar y bloquear (Fase 5) |
| Árbol de lugares (favoritos y unidades) | Parcial | Ubuntu (captura) | Unidades de red con tiempo de espera; favoritos editables |
| Navegación e historial, barra de ruta con migas | Parcial | Ubuntu | Verificación manual con teclado y ratón |
| Vista detallada virtualizada con orden natural | Parcial | Ubuntu: 100 000 archivos en ~365 ms, primeras filas a 21 ms | Medir el bloqueo del hilo de interfaz; columnas opcionales |
| Tamaño de carpetas | Pendiente | — | — |
| Operaciones básicas (crear, copiar, mover, renombrar, eliminar) | Pendiente | — | Módulos `ops` y `journal` |
| Papelera propia | Pendiente | — | Módulo `trash` |
| Historial con deshacer y recuperación | Pendiente | — | Módulo `journal` |
| Atajos principales | Parcial | — | Los de navegación y pestañas existen; faltan los de operaciones |
| Manejo de errores visible | Parcial | Pruebas en CI | Carpeta ilegible muestra mensaje y "Reintentar" |
| Búsqueda | Pendiente (Fase 2) | — | El campo aparece deshabilitado con el rótulo "próximamente" |

## Fases 2 a 6
Todas **Pendientes**. Plan y criterios en `docs/PHASE_PLAN.md`.
