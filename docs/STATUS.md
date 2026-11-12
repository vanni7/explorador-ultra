# Estado del proyecto

Última actualización: 2026-11-12 · Fase actual: **1 (en curso)**; la Fase 0 fue aprobada

Estados: **Terminada** (cumple §26) · **Parcial** · **Pendiente** · **Bloqueada**.

## Punto de reanudación (se actualiza tras cada paso; si hubo un corte, empieza aquí)
Sesión del 2026-11-12, rama `fase-1/core`. Cada paso terminado queda en un commit ya enviado a GitHub; lo que no esté en `git log` no está hecho. Antes de seguir: `git status` (puede haber cambios a medias sin confirmar) y compilar con el preset `release`.

| # | Paso | Estado |
|---|---|---|
| 1 | Revisar la CI de `journal`, `ops`, `trash` y el motor | **Hecho**: en verde en Ubuntu 22.04/24.04, ASan, Windows y macOS |
| 2 | `ui::FileActions`: ejecutar el motor en tareas y devolver el resultado a la interfaz | **Hecho** (6 pruebas en `test_ui_file_actions`, Ubuntu) |
| 3 | Menú contextual, atajos, renombrado en línea, portapapeles, progreso y errores | **Hecho** (pruebas en `test_ui_main_window`, Ubuntu release y ASan). Sin probar a mano con teclado y ratón; falta ver la CI |
| 4 | Arrastrar y soltar | En curso |
| 5 | Diálogo de recuperación al arrancar | Pendiente |
| 6 | Vista de la papelera | Pendiente |
| 7 | Tamaño de carpetas | Pendiente |
| 8 | Verificación manual (Ubuntu y VM de Windows), fusionar a `main`, informe de cierre | Pendiente |

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

## Fase 1 — Explorador funcional (en curso; pausada el 2026-11-11)
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
| `journal`: historial persistente (write-ahead) | Parcial | Ubuntu y VM de Windows (pruebas locales) | **CI pendiente**; retención automática al arrancar |
| `ops`: operaciones elementales (copia atómica, renombrar sin sobrescribir, borrado sin seguir enlaces) | Parcial | Ubuntu (13 pruebas, ASan, TSan) | **CI pendiente** (Windows y macOS sin probar) |
| `trash`: papelera propia por volumen | Parcial | Ubuntu: raíz de volumen (tmpfs) y carpeta personal (disco del sistema) | **CI pendiente**; vista unificada de la papelera en la interfaz; retención y tamaño máximo |
| Motor de operaciones (`ops::Engine`): crear, renombrar, copiar, mover (también entre unidades), papelera, eliminar, deshacer | Parcial | Ubuntu (16 pruebas, ASan, TSan), con movimiento real entre tmpfs y ext4 | **CI pendiente**; **no está conectado a la interfaz**; sobrescribir y demás políticas de colisión (Fase 4) |
| Recuperación tras cierre inesperado | Parcial | Ubuntu: proceso matado a mitad de mover y de copiar; revertir y descartar | "Reanudar" no existe; falta el diálogo al arrancar |
| Operaciones en la interfaz (menú contextual, atajos, portapapeles, arrastrar y soltar, progreso, diálogos) | **Pendiente** | — | Es el siguiente paso |
| Tamaño de carpetas | Pendiente | — | — |
| Atajos principales | Parcial | — | Los de navegación y pestañas existen; faltan los de operaciones |
| Manejo de errores visible | Parcial | Pruebas | Carpeta ilegible muestra mensaje y "Reintentar"; faltan los errores de operaciones |
| Búsqueda | Pendiente (Fase 2) | — | El campo aparece deshabilitado con el rótulo "próximamente" |

### Dónde quedó (2026-11-11)
- Último commit de la rama: el motor de operaciones con sus pruebas. Todo compila y las 15 pruebas pasan en local (release y ASan; 13 con TSan).
- La última ejecución de la CI que terminó es anterior a `journal`, `ops`, `trash` y el motor: esos módulos **no han pasado aún por Windows ni macOS en la CI**. Lo primero al retomar es mirar `gh run list --limit 3`.
- La rama `fase-1/core` no está fusionada a `main`.
- La VM `EU-Win11-Build` queda apagada.

## Fases 2 a 6
Todas **Pendientes**. Plan y criterios en `docs/PHASE_PLAN.md`.
