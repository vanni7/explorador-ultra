# Informe de la Fase 2 — Búsqueda y rendimiento

Fecha: 2026-11-14 · Estado: **entregada con puntos abiertos**. El explorador busca por nombre, tamaño, fecha, tipo y expresión regular, en la carpeta, en sus subcarpetas o en unidades enteras; tiene un índice opcional que se mantiene al día solo, y el planificador adapta la concurrencia a cada unidad. Las pruebas están en verde en las tres plataformas. Según el criterio de §26, ninguna función queda "Terminada": falta que el dueño pruebe el índice y los alcances, nadie ha usado las ventanas a mano en Windows ni en macOS, y hay mediciones pendientes (apartado E). El detalle paso a paso está en [`STATUS.md`](STATUS.md).

## A. Qué hay
| Área | Qué hace |
|---|---|
| Sintaxis de búsqueda | Comodines, `AND`/`OR`/`NOT`, paréntesis, comillas, filtros `ext:` `size:` `modified:` `created:` `type:` `hidden:` `name:` `content:` y `regex:`. Documentada en [`SEARCH_SYNTAX.md`](SEARCH_SYNTAX.md) |
| Filtro local | Ctrl+I: deja a la vista lo que cumple, a cada tecla |
| Buscador | Ctrl+F: resultados progresivos como una ubicación más de la pestaña, con pausa, detención y contadores. Conserva lo escrito al cambiar de carpeta y lista las 15 últimas búsquedas |
| Alcances | Solo la carpeta, con subcarpetas, toda la unidad, todas las unidades o las elegidas. Desde «Unidades» se busca en todas |
| Opciones | Distinguir mayúsculas; comparar el nombre o el contenido de archivos de texto |
| Índice | Opcional, por unidad, en SQLite. Asistente de primer arranque, ventana «Índice de búsqueda», resultados marcados «Índice»/«Disco» y «Verificar en disco» |
| Vigilancia de cambios | inotify (Linux, con tope del 60 % del límite y repaso cada 30 min de lo no vigilado), ReadDirectoryChangesW (Windows), FSEvents (macOS) |
| Concurrencia | Límite de tareas simultáneas por unidad según su clase, con tres niveles de urgencia |
| Añadidos pedidos por el dueño | Ubicación «Unidades» con tamaño, ocupado y libre; flechas del teclado dentro del árbol |

## B. Criterios de aceptación de la fase (`PHASE_PLAN.md`)
| Criterio | Estado |
|---|---|
| Gramática documentada; el analizador pasa pruebas de comodines, operadores, paréntesis, todos los filtros de §6.1 y `regex:` | **Cumplido** (`test_search_query`: 59 casos aprobados, contando las filas de datos). `tag:` se acepta pero avisa de que aún no funciona (Fase 4) |
| Resultados progresivos como ubicación virtual, con pausa, cancelación (< 500 ms) y contadores | **Cumplido** en pruebas automáticas; el dueño lo usó a mano en Ubuntu |
| Filtrado local de 100 000 elementos < 50 ms por tecla en este equipo | **Cumplido**: 5–7 ms |
| Índice SQLite opcional con asistente de primer arranque; actualización por vigilancia de cambios; con 1 000 000 de entradas, primeros resultados < 200 ms (medido) | **Cumplido en este equipo cuando hay resultados** (5–76 ms hasta el primero, árbol sintético, caché caliente). Una consulta sin pista de nombre y sin resultados tarda 0,8–1,2 s en terminar |
| Resultados "del índice" distinguibles y verificables; aviso y guía si el límite de inotify no alcanza | **Cumplido**; visto con ratón simulado. En este equipo el límite no alcanza (65 536 frente a 135 517 carpetas solo en una unidad) y la ventana lo explica |
| NFC/NFD: una búsqueda encuentra el mismo nombre en ambas normalizaciones | **Cumplido** (pruebas) |
| `tasks` completo, con concurrencia adaptativa por dispositivo | **Cumplido en pruebas**; sin medir en un disco mecánico real |

## C. Verificación realizada
- **CI** (GitHub Actions): Ubuntu 22.04 y 24.04, Ubuntu con ASan+UBSan, Windows (MSVC), macOS Intel y Apple Silicon.
- **Local**: 22 programas de prueba en release y ASan+UBSan; 20 en TSan (sin las pruebas con ventana).
- **VM de Windows 11**: las 22 pruebas y el arranque (13-nov).
- **Pantalla virtual X11 con ratón y teclado simulados**: búsqueda real en `/media/merwin/RESPALDO`, menú de opciones, flechas del árbol, búsquedas recientes, asistente del índice, indexación de una unidad real, resultados del índice y ventana del índice.
- **Prueba manual del dueño** en Ubuntu 26.04 (Wayland): el filtro, la búsqueda en subcarpetas y la vista de unidades. Salieron cuatro observaciones, todas atendidas (apartado D). **No ha probado todavía** los alcances, la búsqueda en contenido, el índice ni las búsquedas recientes.

## D. Fallos y observaciones durante la fase
| Qué | Cómo apareció | Estado |
|---|---|---|
| `walter riso*` no encontraba «WALTER RISO - ….pdf»: el espacio partía la consulta y `riso*` exigía que el nombre empezara por «riso» | El dueño, al usarlo | Corregido: varias palabras con comodines valen también como frase |
| En el árbol, las flechas actuaban sobre la lista de archivos | El dueño | Corregido: el clic ya no quita el teclado al árbol |
| No había opción de mayúsculas ni de buscar en el contenido | El dueño | Hecho; el contenido solo en archivos de texto |
| Al cambiar de carpeta se borraba lo buscado | El dueño | Corregido, más lista de búsquedas recientes |
| En Windows un nombre con punto inicial no es oculto; en macOS la carpeta temporal no tiene papelera y un diálogo colgaba la prueba | CI | Corregidas las pruebas |
| La raíz del índice se guardaba como NULL y la sincronización no terminaba | Prueba propia, antes de enviar | Corregido |
| Con un solo hilo de indexación, buscar habría esperado a que terminara de indexarse una unidad | Revisión al escribir `IndexService` | Corregido: tres hilos, el índice usa uno |
| El índice habría agotado las vigilancias inotify de todo el usuario | Revisión, al ver el límite de este equipo | Corregido: tope del 60 % y repaso periódico |
| Cancelar una tarea que esperaba un disco ocupado no la soltaba | Revisión al escribir el limitador | Corregido, con prueba |

## E. Lo que queda abierto
1. **Que el dueño pruebe** alcances, contenido, índice, búsquedas recientes y concurrencia.
2. **Usar las ventanas a mano en Windows y macOS.** Allí solo han corrido las pruebas automáticas.
3. **Búsqueda en contenido**: solo archivos de texto de hasta 20 MB. PDF y ofimática, con índice FTS5, en la Fase 4.
4. **Mediciones que faltan**: caché fría, discos mecánicos, USB y red; bloqueo del hilo de interfaz con cientos de miles de resultados; el efecto real del limitador en un disco mecánico; el índice en Windows y macOS.
5. **Búsqueda en varias unidades**: se recorren una detrás de otra en una sola tarea (§6.3 pide que sea concurrente por dispositivo), y el índice solo se usa si todas las unidades lo tienen.
6. **Índice**: una consulta sin pista de nombre lee todas las filas; no hay fanotify ni el servicio de Windows para MFT/USN (Fase 5); con la aplicación cerrada no se siguen los cambios (se repasa al arrancar).
7. **Concurrencia**: medida el 15-nov en los discos reales (apartado de `BENCHMARKS.md`). La medición mostró que el límite de tareas no bastaba y se añadió que las tareas largas cedan el disco a lo interactivo (abrir carpetas con una lectura grande en marcha: de 11 s a 0,3 s en el disco mecánico). Siguen sin medir escrituras, USB y red. Una tarea de fondo que ya empezó no cede el disco a una copia que llega después; los límites se cambian en el archivo de configuración, sin pantalla de ajustes.
8. Desde los resultados no se puede renombrar ni soltar archivos.
9. **Fallo intermitente sin explicar**: `test_ui_main_window` falló una vez de tres en el Windows de la CI después del paso 6, sin dejar salida (lo más probable es que el proceso terminara de golpe). Pasó al relanzarlo y cinco veces seguidas en la VM de Windows. No se ha encontrado la causa; la CI repite ahora con detalle cualquier prueba que falle.
10. Siguen abiertos los puntos del [informe de la Fase 1](INFORME_FASE_1.md).

## F. Siguiente fase
Fase 3 — Miniaturas, previsualización y metadatos. Plan y criterios en [`PHASE_PLAN.md`](PHASE_PLAN.md).
