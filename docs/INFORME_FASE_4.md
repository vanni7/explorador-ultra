# Informe de la Fase 4 — Herramientas

Fecha: 2026-11-16 · Estado: **entregada con puntos abiertos**. El explorador navega y crea comprimidos, busca dentro de PDF y documentos de oficina (con índice de texto completo opcional), tiene una cola de transferencias con pausa, reintento, reanudación y comprobación por hash, renombra en masa, organiza carpetas, encuentra duplicados e imágenes parecidas y etiqueta archivos. Según el criterio de §26, ninguna función queda "Terminada": falta que el dueño la use, nadie ha usado las ventanas a mano en Windows ni en macOS y quedan mediciones (apartado E). El detalle paso a paso está en [`STATUS.md`](STATUS.md).

## A. Qué hay
| Área | Qué hace |
|---|---|
| Comprimidos (`archive`) | Entrar en ZIP, 7z, TAR, TAR.GZ, RAR e ISO como si fueran carpetas; vista previa y abrir lo de dentro; extraer (todo, lo seleccionado, en carpeta nueva); comprimir en ZIP, 7z o TAR.GZ. Defensas propias contra rutas que salen (zip-slip), enlaces que escapan y bombas |
| Búsqueda en contenido (`content`, `search`) | Texto de TXT y código, PDF, DOCX, XLSX, PPTX, ODT/ODS/ODP y RTF, leído en el proceso auxiliar; columna «Fragmento» con la coincidencia resaltada y la página, diapositiva o línea |
| Índice de contenido (`index`) | Opcional, por carpeta, en SQLite FTS5 con trigramas: buscar en 48 libros pasa de 19,4 s a 25 ms. Nunca da resultados viejos: lo cambiado se lee del disco |
| Transferencias (`ops`, `ui`) | Cola con pausa, cancelar, reintentar (sin duplicar), pasar delante, de una en una o en paralelo, velocidad y tiempo restante; todas las políticas de colisión con ventana de «Ya existe»; reemplazar manda lo pisado a la papelera; comprobación con SHA-256 (SHA-1, MD5, CRC32); reanudación desde punto de control y tras un cierre inesperado |
| Renombrado masivo (`rename`) | F2 con varios seleccionados: reglas en cadena (mayúsculas, sustituir, eliminar, insertar, regex, contador, plantilla con fecha de la foto, cámara, etiquetas de música…), vista previa con conflictos y ciclos, deshacer en bloque |
| Organizador (`organize`) | Ordenar en subcarpetas por tipo (reglas de §12), fecha, fecha de la toma, tamaño o artista/álbum; reglas editables y guardables; simulación obligatoria; deshacer en bloque |
| Duplicados (`dupes`) | Por etapas (tamaño, hash parcial, SHA-256, byte a byte opcional), con enlaces físicos aparte; marcar por reglas; nunca todas las copias; a la papelera tras confirmar |
| Imágenes parecidas (`similar`) | dHash de 128 bits, umbral ajustable, comparación lado a lado con resolución, tamaño y fecha; marcado manual |
| Etiquetas (`tags`) | Base local con colores; siguen a los archivos al mover, renombrar, deshacer y papelera; reconocen lo movido por fuera por su identidad; huérfanas revisables sin adivinar; `tag:` en la búsqueda |
| Menú nuevo | **Herramientas**: Renombrar varios, Organizar esta carpeta, Buscar duplicados, Imágenes parecidas, Etiquetas |

## B. Criterios de aceptación de la fase (`PHASE_PLAN.md`)
| Criterio | Estado |
|---|---|
| Renombrado masivo con vista previa, detección de conflictos y ciclos, por metadatos, y deshacer en bloque | **Cumplido** (`test_rename`, prueba de ventana con F2; visto con 8 fotos reales renombradas por fecha de la toma y cámara, y deshechas con Ctrl+Z) |
| Organizador con simulación obligatoria y deshacer en bloque | **Cumplido** («Organizar» solo se habilita con la simulación de lo que se ve; `test_organize` y prueba de ventana) |
| Cola de transferencias con pausa, reanudación, verificación SHA-256, todas las políticas de colisión; copia ≥ 90 % de la velocidad nativa (medido) | **Cumplido en Linux**: 100 % SSD→SSD, 97 % SSD→HDD, 97 % HDD→HDD frente a `cp` + `sync` (2 GB, caché fría). Sin medir en Windows, macOS, USB ni red |
| Duplicados por etapas, distinguiendo enlaces físicos; nunca borra solo | **Cumplido** (`test_dupes`; en `BIBLIOTECA_DIGITAL`, 20,9 GB recuperables encontrados en 90 s) |
| Imágenes similares con umbral ajustable y comparación lado a lado | **Cumplido** (`test_similar`; con fotos reales, una versión reducida y recomprimida al 99 % de parecido) |
| Etiquetas en base local que sobreviven a mover y renombrar; huérfanas revisables | **Cumplido** (`test_tags`, prueba de ventana con renombrar y deshacer) |
| Comprimidos: navegar, extraer, crear ZIP/7z/TAR/TAR.GZ; RAR e ISO solo lectura; zip-slip y bombas en verde | **Cumplido**, salvo RAR real (libarchive no crea RAR y no hay ninguno en el equipo para probar) |
| Búsqueda en contenido en los formatos de §6.5, con fragmento resaltado y límites por archivo | **Cumplido** (PDF reales; DOCX, XLSX, PPTX, ODT y RTF fabricados en las pruebas) |

## C. Verificación realizada
- **CI** (GitHub Actions): Ubuntu 22.04 y 24.04, Ubuntu con ASan+UBSan, Windows (MSVC), macOS Intel y Apple Silicon (este último pasa de `macos-latest`, que se quedaba sin máquina, a `macos-15`).
- **Local**: 32 programas de prueba en release y con ASan+UBSan; TSan en el índice de contenido y el buscador.
- **Pantalla virtual X11 con ratón y teclado simulados**: cada ventana nueva con datos reales del equipo (los 48 PDF de Walter Riso, fotos de `Imágenes`, `BIBLIOTECA_DIGITAL`).
- **Mediciones** en los discos reales: búsqueda en contenido, índice de contenido, copia frente a `cp`, hashes y duplicados (`BENCHMARKS.md`).
- **No** se ha usado a mano en Windows ni en macOS, y el dueño aún no lo ha probado (recordatorio puesto para el 17-nov a las 7:30).

## D. Fallos y observaciones durante la fase
| Qué | Cómo apareció | Estado |
|---|---|---|
| Dentro de un comprimido el panel de vista previa mostraba una ruta ilegible y «no existe» | Pantalla virtual | Corregido: se extrae esa entrada a un temporal |
| Las palabras partidas con guion en los PDF no se encontraban («afrontamiento») | Medición con PDF reales | Corregido: se quitan las marcas de guion de PDFium |
| El fragmento quedaba fuera de la vista o con la coincidencia cortada | Pantalla virtual | Corregido: columna tras el nombre, poco contexto delante |
| Una acción cancelada antes de empezar nunca avisaba de que había terminado (venía de antes) | Prueba nueva de la cola | Corregido |
| Pegar sobre nombres existentes ahora pregunta: dos pruebas antiguas se quedaban esperando la ventana | Pruebas | Pruebas ajustadas para responder |
| Reintentar con «Omitir» se saltaba la carpeta que la propia copia había empezado | Prueba de reanudación | Corregido: al reintentar, las carpetas se completan |
| Un punto de control cada 64 MB dejaba la copia a disco mecánico en el 81 % de `cp` | Medición | Corregido: como mucho cada 8 s (97 %) |
| El SHA-256 de Qt (162 MB/s) dejaba la copia comprobada en 65 MB/s | Medición | OpenSSL opcional (2088 MB/s): 171 MB/s |
| `{tamaño}` y «Resolución» no se reconocían (comparación de «ñ»/«ó» como Latin-1) | Prueba del organizador | Corregido |
| F2 con varios seleccionados renombraba solo uno | Pantalla virtual | Corregido; la prueba ahora pulsa F2 de verdad |
| Una prueba daba por válido «:» con las reglas nativas (no lo es en macOS ni Windows) | CI | Prueba corregida |
| «Omitir si son idénticos» dejaba otra copia numerada si ya existía «(2)» idéntico | Pantalla virtual | Corregido |
| La columna de etiquetas quitaba sitio al nombre | Pantalla virtual | Solo aparece si algo de la carpeta tiene etiquetas |
| Tras pegar varios, a veces quedaba seleccionado solo uno: si al terminar la copia la lista aún no mostraba todos (el aviso del sistema de alguno no había llegado), se seleccionaba lo que había y se dejaba de esperar | CI de Windows, dos veces seguidas en `copyCutPasteAndUndo` | Corregido: se sigue esperando a los que faltan y se vuelve a leer la carpeta |
| Cerrar la pestaña de una búsqueda (su ✕) cerraba la aplicación si era la única pestaña | El dueño, al probar (17-nov) | Corregido: la ✕ de una búsqueda vuelve a la carpeta donde se empezó; otra ✕ ya cierra la pestaña |
| Mientras una carpeta se volvía a leer (tras un cambio en el disco), soltar archivos en ella se rechazaba | CI de macOS (`dragAndDrop`): allí el aviso del disco llega tarde y coincide | Corregido: durante la relectura la lista conserva sus filas (también con dos avisos seguidos del disco, que antes la dejaban en blanco un instante) y se puede soltar como siempre. Pruebas nuevas |
| Con varios escritores a la vez y un disco lento, una escritura del historial podía fallar por «base ocupada» (y la operación no se habría hecho) | CI de Windows (`manyThreadsWriteAtOnce`; ya falló una vez en la Fase 3) | Corregido: las escrituras se reintentan si SQLite dice ocupada, y la espera sube a 30 s. Importa más ahora que la cola puede llevar varias transferencias a la vez |
| Las pruebas de duplicados y de imágenes parecidas se colgaban en macOS: allí la carpeta temporal no tiene papelera y la ventana ofrecía borrar del todo con un diálogo | CI | Pruebas corregidas (se saltan sin papelera, como las de la Fase 2) |

## E. Lo que queda abierto
1. **Que el dueño lo pruebe** todo: comprimidos, búsqueda en documentos, índice de contenido, cola de transferencias, renombrado, organizador, duplicados, parecidas y etiquetas.
2. **Windows y macOS a mano.** Allí solo han corrido las pruebas automáticas; en ellos la comprobación de copias usa el SHA-256 de Qt si no hay OpenSSL, y en Windows la copia comprobada relee de la caché.
3. **Mediciones que faltan**: copia en Windows y macOS, USB y red; índice de contenido con miles de documentos; parecidas con más de 50 000 imágenes (hoy todas contra todas).
4. **Comprimidos**: miniaturas de lo de dentro, añadir o quitar dentro de un comprimido, deshacer extraer y comprimir, RAR real.
5. **Pendientes menores**: la comprobación final no cuenta en la barra de avance; el temporal `.eu-parcial` se ve mientras se copia; abrir un PDF en la página del fragmento; guardar reglas favoritas del renombrado; filtrar por etiqueta en el panel lateral; reanudar operaciones anteriores al esquema 2 del historial.
6. **Fallo intermitente** de `copyCutPasteAndUndo` en Windows (apartado D).
7. Siguen abiertos los puntos de los informes anteriores.

## F. Siguiente fase
Fase 5 — Funciones profesionales. Plan y criterios en [`PHASE_PLAN.md`](PHASE_PLAN.md).
