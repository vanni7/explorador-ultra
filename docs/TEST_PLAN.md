# Plan de pruebas

## Niveles
| Nivel | Qué cubre | Herramienta | Dónde corre |
|---|---|---|---|
| Unitarias | Funciones puras de cada módulo (orden natural, analizador de consultas, reglas de renombrado, validación de nombres) | Qt Test | Local + CI (3 plataformas) |
| Integración | Varios módulos sobre archivos reales en `QTemporaryDir`: `ops`+`journal`+`trash`, `index`+vigilancia, `archive` | Qt Test | Local + CI |
| Concurrencia y cancelación | Planificador, cancelar a mitad, progreso | Qt Test + TSan | Local + CI (Linux) |
| Recuperación ante fallos | Proceso hijo que se mata (`SIGKILL`/`TerminateProcess`) a mitad de una operación; al reabrir, el historial debe detectar y resolver | Qt Test + guion | Local + CI |
| Rendimiento | Escenarios de §4 | `eu-bench`, `QBENCHMARK`, hyperfine | Local (resultados a `BENCHMARKS.md`) |
| Interfaz (automática) | Modelos y vistas con `QAbstractItemModelTester`, atajos, foco | Qt Test, plataforma `offscreen` | Local + CI |
| Interfaz (manual) | Lista de comprobación por fase | Persona | Ubuntu (Wayland y X11) y VM de Windows |
| Análisis | ASan, UBSan, TSan, clang-tidy | presets `asan`, `tsan` | CI (Linux) |

## Casos obligatorios (§22)
| Caso | Cómo se prueba | Fase |
|---|---|---|
| Más de 100 000 archivos | `eu-bench mktree` + pruebas del modelo de la vista | 1 |
| Archivos grandes (> 4 GB) | Archivos dispersos (*sparse*) para no gastar disco, más uno real en pruebas de copia | 1, 4 |
| Rutas largas (> 260 caracteres) | Árbol profundo generado; en Windows con prefijo `\\?\` | 1 |
| Unicode: NFC/NFD, emoji, texto de derecha a izquierda | Nombres generados en ambas normalizaciones; comparar búsqueda y orden | 1, 2 |
| Permisos | Carpetas sin lectura o escritura (`chmod`, ACL en Windows); no se ejecuta como administrador | 1 |
| Enlaces simbólicos y ciclos | `a/b -> a`; eliminar nunca sigue el enlace; el recorrido termina | 1 |
| Mover entre volúmenes | Segundo sistema de archivos montado en bucle (`tmpfs` / imagen); en Windows, un VHD | 1 |
| Interrupción a mitad de operación | Matar el proceso en puntos de inyección; verificar que no se pierde ni duplica nada | 1, 4 |
| Comprimidos maliciosos | Muestras generadas por guion: `../`, rutas absolutas, enlaces que escapan, bomba de proporción y de número de entradas | 4 |
| Archivos dañados | Imágenes, PDF, videos y comprimidos truncados; nada debe colgarse ni cerrarse | 3, 4 |
| Desconexión de unidad | Desmontar la imagen en bucle durante listado y copia | 1, 4 |
| Papelera por volumen | Mismo dispositivo verificado por identificador; restaurar sin base de datos | 1 |

## Reglas
- Ninguna prueba toca datos reales: todo vive en directorios temporales que la propia prueba crea y borra.
- Una función no pasa a "Terminada" sin pruebas automatizadas en verde en la CI y verificación manual en Ubuntu (§26).
- En macOS solo hay CI: la interfaz se marca "no verificada manualmente en macOS".
- X11 se prueba iniciando sesión "GNOME en Xorg" o con `QT_QPA_PLATFORM=xcb` (XWayland); arrastrar y soltar se prueba en ambos.

## Qué cubre cada sanitizer
- **ASan + UBSan** (preset `asan`, también en la CI): todas las pruebas.
- **TSan** (preset `tsan`, local): todas menos las que crean una `QApplication` (`test_ui_directory_model`, `test_ui_main_window`). Los binarios oficiales de Qt y libdbus no están instrumentados y TSan informa de carreras dentro de ellos que no son del proyecto.

## Lista manual de la Fase 0 (hecha)
- [x] La ventana mínima abre en Ubuntu 26.04 (Wayland) y muestra versión de Qt e icono.
- [x] Compila, pasa las pruebas y arranca con `--smoke-test` (plataforma `offscreen`) en la VM de Windows.
- [ ] Ver la ventana mínima en el escritorio de Windows (no se logró lanzarla en la sesión gráfica desde Ubuntu).
