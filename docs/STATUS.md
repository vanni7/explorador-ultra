# Estado del proyecto

Última actualización: 2026-11-11 · Fase actual: **0 (entregada, pendiente de aprobación)**

Estados: **Terminada** (cumple §26) · **Parcial** · **Pendiente** · **Bloqueada**.

## Fase 0 — Arquitectura y entorno
| Tarea | Estado | Verificado en | Qué falta |
|---|---|---|---|
| Entorno en Ubuntu (compiladores, CMake, Qt 6.12, bibliotecas) | Terminada | Ubuntu 26.04 | — |
| "Hola mundo" Qt: compila, pasa pruebas y arranca | Parcial | Ubuntu 26.04 (Wayland y XWayland; ASan+UBSan) y Windows 11 en la VM (MSVC; arranque con plataforma `offscreen`) | Ver la ventana en el escritorio de Windows; macOS (CI) |
| VM de Windows `EU-Win11-Build` | Terminada | Compila y pasa las pruebas dentro de la VM | Falta Qt PDF (Fase 3) |
| Repositorio Git local | Terminada | — | — |
| Repositorio en GitHub y CI de las tres plataformas | **Bloqueada** | — | El usuario debe autenticar `gh` (`gh auth login`). El flujo `.github/workflows/ci.yml` está escrito pero **nunca se ha ejecutado** |
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

## Fases 1 a 6
Todas **Pendientes**. Plan y criterios en `docs/PHASE_PLAN.md`. La Fase 1 no empieza hasta que el usuario apruebe el informe de la Fase 0.
