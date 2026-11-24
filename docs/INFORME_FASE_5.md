# Informe de la Fase 5 — Funciones profesionales

Fecha: 2026-11-23 · Estado: **entregada con puntos abiertos**. El explorador suma columnas Miller, doble panel, comparación y sincronización, pestañas fijadas y bloqueadas con sesión completa, personalización, impresión y un servicio opcional de Windows que evita recorrer la unidad al arrancar.

## A. Qué hay
| Área | Qué hace |
|---|---|
| Columnas Miller | Una columna por carpeta de la ruta, con teclado, anchos ajustables y vista previa (Ctrl+3) |
| Doble panel | Dos grupos de pestañas independientes (F3), cambiar con F6, copiar y mover al otro panel, arrastrar entre ellos |
| Comparar y sincronizar | Por nombre, tamaño, fecha, SHA-256 o contenido; en uno o en dos sentidos; simulación obligatoria; no propaga borrados sin política explícita |
| Pestañas y sesión | Fijar, bloquear, duplicar, reabrir cerradas; la sesión guarda pestañas, vistas, paneles y anchos |
| Personalización | Temas claro, oscuro y personalizado; atajos con detección de conflictos; exportar, importar y restaurar la configuración |
| Impresión | Vista previa de imagen, PDF, texto, código y documentos (Ctrl+P) |
| Servicio de Windows (opcional) | `eu-indexsvc`: lee el diario de NTFS, solo lectura, con instalación y desinstalación explícitas; la aplicación funciona igual sin él (ADR-0013) |
| Pedidos del dueño durante la fase | Organizar quita carpetas vacías; menú Vista; vista previa de CSV; reproductor de video y audio con sonido; Ctrl + rueda en miniaturas; soltar sobre carpetas en miniaturas; tamaño debajo del nombre en miniaturas; registro de fallos |

## B. Criterios de aceptación de la fase (`PHASE_PLAN.md`)
| Criterio | Estado |
|---|---|
| Columnas Miller navegables por teclado; doble panel con arrastre entre paneles | **Cumplido** en pruebas automáticas y pantalla virtual. Falta confirmar F3 y F6 en el escritorio real |
| Comparación y sincronización uni y bidireccional, simulación obligatoria, sin propagar borrados sin política | **Cumplido** (pruebas automáticas) |
| Sesión completa restaurada; pestañas fijadas y bloqueadas | **Cumplido** (`tabsPinLockReopenAndFullSession`) |
| Temas al instante; atajos con conflictos; exportar e importar | **Cumplido** (pruebas automáticas); falta que el dueño lo pruebe |
| Impresión con vista previa de imagen, PDF, texto y código | **Cumplido** imprimiendo a PDF; **sin probar en una impresora real** |
| Servicio de Windows: elevación explícita, solo lectura, la aplicación funciona igual sin él, verificado en la VM | **Cumplido en la VM y en la CI** (instala, responde, reinstala, desinstala y no queda). El aviso de administrador real **no se ha visto**: la VM tiene UAC desactivado |

## C. Verificación realizada
- **CI** (`54a1202`): Ubuntu 22.04 y 24.04, Ubuntu con ASan+UBSan, Windows (MSVC), macOS Intel y Apple Silicon, todo en verde.
- **VM de Windows 11**: 36/36 pruebas, con la instalación real del servicio y el arranque de la aplicación sin pantalla.
- **Ubuntu**: 35/35 pruebas en release.
- **Medición del servicio** en la VM, unidad C: con unas 362 900 entradas: el diario responde en 0–1 ms; recorrer la unidad tarda 20,1 s la primera vez y unos 11 s con la caché caliente (`BENCHMARKS.md`).

## D. Fallos encontrados al cerrar la fase
| Qué | Cómo apareció | Estado |
|---|---|---|
| Con la carpeta de datos dentro de lo indexado, el índice de Windows se releía sin fin | CI de Windows | Corregido: la modificación de una carpeta ignorada ya no marca a su madre |
| El primer arreglo dejó sin aviso las modificaciones de archivos | CI de Windows (`test_index_tree_watcher`) | Corregido antes de seguir |
| Un renombrado masivo podía dejar archivos sin renombrar si un programa los tenía abiertos un instante («en uso») | VM con la CPU saturada: 2 de 68 vueltas | Corregido: reintenta unos 2 s; 0 de 82 vueltas. Qué programa lo abría no se identificó |
| La aplicación se cerró una vez al terminar de organizar (20-nov) | El dueño | **Sin repetir ni explicar.** El registro de fallos (`registros/fallos.log`) lo capturará si vuelve a pasar |

## E. Lo que queda abierto
1. **Que el dueño lo pruebe**, sobre todo sincronización, personalización y atajos F3/F6.
2. **Imprimir en una impresora real.**
3. **Ver el aviso de administrador** al instalar el servicio en un Windows con UAC activo.
4. **Medir** el arranque completo con el índice y el servicio, en un disco grande y con caché fría (lo medido es la consulta al diario frente al recorrido).
5. El cierre inesperado del 20-nov (apartado D).
6. Siguen abiertos los puntos de los informes anteriores.

## F. Siguiente fase
Fase 6 — Optimización y distribución. Plan y criterios en [`PHASE_PLAN.md`](PHASE_PLAN.md).
