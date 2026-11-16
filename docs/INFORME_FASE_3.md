# Informe de la Fase 3 — Miniaturas, previsualización y metadatos

Fecha: 2026-11-16 · Estado: **entregada con puntos abiertos**. El explorador muestra miniaturas de imágenes, PDF, textos, carpetas, videos, audio, documentos de oficina y fotos RAW; tiene un panel con los datos y una vista previa de lo seleccionado; reproduce video y audio dentro de la miniatura, y decodifica todo lo de riesgo en un proceso aparte. Las pruebas están en verde en las tres plataformas. Según §26, nada queda "Terminado": **el dueño aún no ha usado esta fase**, nadie la ha usado a mano en Windows ni en macOS, y las fotos RAW no se han probado con archivos reales. El detalle paso a paso está en [`STATUS.md`](STATUS.md).

## A. Qué hay
| Área | Qué hace |
|---|---|
| Vista de miniaturas | Ver → Detalles (Ctrl+1) / Miniaturas (Ctrl+2), tres tamaños. Mismo modelo y misma selección que la vista detallada: cambiar de vista no pierde la carpeta ni lo seleccionado. Renombrar, arrastrar, soltar y menú contextual iguales |
| Qué se ve en cada miniatura | Imágenes (JPEG, PNG, GIF, WebP, TIFF, SVG, ICO…) giradas según EXIF y sin agrandar; RAW con la vista previa de la cámara; PDF (primera página); texto, código y Markdown (una hoja con las primeras líneas); carpetas (hasta cuatro miniaturas de su contenido si es de una sola clase); video (fotograma del segundo 3, con duración); audio (carátula o una genérica, con duración); documentos de oficina (la miniatura que llevan dentro o, si no, LibreOffice) |
| Caché | En memoria (256 MB) y en disco (1 GB), ambos configurables en `[miniaturas]`; clave ruta + tamaño + fecha + lado; recuerda los archivos que fallaron; al arrancar se borra lo que lleva más tiempo sin verse |
| Carga | Solo lo que se ve; lo que deja de verse al desplazarse se cancela; las fotos van antes que los documentos y los videos; en un disco lento cede el paso a abrir carpetas |
| Reproducción en la miniatura | Botón ▶ al pasar el puntero o barra espaciadora; video en silencio con barra de avance, audio con sonido; se para al salir el puntero; uno solo a la vez por ventana; se puede desactivar en el menú Ver |
| Panel de previsualización | Alt+P. Arriba: tipo, tamaño, fechas, ubicación y los datos propios del tipo (dimensiones y EXIF —cámara, objetivo, fecha de la foto, exposición, apertura, ISO, focal, GPS—; duración, códecs, resolución, canales y etiquetas del audio y el video; páginas del PDF). Abajo: la imagen grande, la primera página, el fotograma, el texto (primeros 512 kB) o el Markdown formateado. Funciona también sobre los resultados de búsqueda |
| Aislamiento | Miniaturas, vista previa y metadatos se leen en `eu-thumbnailer`, un proceso auxiliar por hilo. Si un archivo lo cierra o lo cuelga (30 s), se marca como fallido y se arranca otro; la aplicación sigue. Límite de 3 GB de memoria en Unix |

Además, en esta fase: medida la concurrencia en los discos reales y añadido que las tareas largas cedan el disco a lo interactivo (de 11 s a 0,3 s para abrir carpetas en un disco mecánico durante una copia); ver `BENCHMARKS.md`.

## B. Criterios de aceptación (`PHASE_PLAN.md`)
| Criterio | Estado |
|---|---|
| Miniaturas de imágenes (incluidos SVG y RAW, con orientación EXIF), PDF, texto, video (segundo 3), audio (carátula) y carpetas | **Cumplido** en pruebas y visto con los archivos del dueño (fotos con EXIF real, biblioteca de PDF, videos de `SOLO_PELIS`, MP3 con carátula). **RAW: sin probar con fotos reales** (no hay en el equipo); solo se comprueba que una falsa no rompe nada. Carpetas de contenido mezclado: icono de carpeta, no la "composición de iconos" que pide §5.5 |
| Ofimática con LibreOffice si existe (proceso aislado con tiempo límite) y alternativa si no | **Cumplido**: miniatura incrustada; LibreOffice en proceso aparte, uno a la vez, 30 s, perfil propio. Sin LibreOffice ni miniatura dentro, icono |
| Caché en disco por ruta + tamaño + fecha, límite de RAM configurable (256 MB); desplazarse rápido cancela lo obsoleto | **Cumplido** (pruebas de caché, de tope de memoria y de cancelación) |
| Reproducción en miniatura de video y audio, con límite de decodificadores | **Cumplido**: un reproductor por ventana; las miniaturas usan como mucho los hilos de su grupo (2–6). Probado el video; el audio con sonido, no (no hay altavoces en las pruebas) |
| Panel de previsualización y metadatos por tipo; archivos dañados o maliciosos no cuelgan ni cierran la aplicación | **Cumplido**: el auxiliar se cierra o se cuelga a propósito en las pruebas y la aplicación sigue. Es aislamiento frente a cierres, **no un entorno restringido**: el auxiliar tiene los permisos del usuario |

## C. Verificación realizada
- **CI**: Ubuntu 22.04 y 24.04, Ubuntu con ASan+UBSan, Windows (MSVC), macOS Intel y Apple Silicon. En Ubuntu 24.04 con Qt Multimedia, Qt PDF, FFmpeg, libarchive y LibRaw (no Exiv2: trae la 0.27 y se exige la 0.28, así que el EXIF solo se prueba en este equipo); en Ubuntu 22.04 sin FFmpeg (4.4, demasiado antiguo); en Windows y macOS sin las bibliotecas opcionales (esas pruebas se saltan y se ve el icono).
- **Local**: 24 programas de prueba en release y ASan; 20 en TSan (sin los que usan la interfaz o el escalado de imágenes de Qt, que no está instrumentado; una supresión acotada documentada en `tests/tsan.supp`).
- **VM de Windows 11**: las 24 pruebas; ahí aparecieron dos fallos (apartado D).
- **Pantalla virtual con ratón simulado**, sobre carpetas reales del dueño: miniaturas de fotos, carpetas de PDF, videos y música; panel con EXIF real; reproducción en la miniatura.

## D. Fallos encontrados durante la fase
| Fallo | Cómo apareció | Estado |
|---|---|---|
| Abrir carpetas durante una lectura grande tardaba 10–50 veces más (disco mecánico y SSD) | Medición en los discos reales | Corregido: las tareas largas ceden el disco |
| La caché de miniaturas podía usarse ya liberada al cerrar la ventana con una miniatura en marcha | Al buscar por qué la prueba de la ventana se cerraba sin salida en el Windows de la CI | Corregido: caché compartida |
| Pruebas que, al fallar a medias, dejaban tareas usando variables destruidas (cierre 0xc0000409) | VM de Windows (allí dormir 1 ms dura ~15 ms) | Corregido |
| En el Windows de la CI las pruebas no dejaban salida | Al no poder ver un fallo | Corregido: las pruebas son programas de consola |
| La prueba de 100 000 archivos pasaba del límite de 5 min de Qt en la CI de Windows | CI | Límite a 15 min |
| Con el límite de memoria mal elegido (`RLIMIT_AS`) el auxiliar no abría ningún PDF; con ASan no arrancaba | Medición y CI | Corregido (`RLIMIT_DATA`, sin límite con sanitizadores) |
| Con una carpeta como argumento no se leían las preferencias | Prueba en pantalla | Corregido |
| En miniaturas solo se seleccionaba una columna: la barra de estado no contaba lo seleccionado | Prueba en pantalla | Corregido, con prueba |
| El panel quedaba con ancho cero con una disposición guardada de antes | Prueba en pantalla | Corregido |

## E. Lo que queda abierto
1. **Que el dueño use la fase**: vista de miniaturas, panel, reproducción (con sonido), documentos de oficina.
2. **Usar las ventanas a mano en Windows y macOS.** Allí, además, la CI no tiene FFmpeg, LibRaw, libarchive ni Exiv2: sin ellos no hay miniaturas de video, RAW ni oficina incrustada, ni EXIF. Hay que empaquetarlas (vcpkg) antes de distribuir (Fase 6).
3. **Fotos RAW con archivos reales.**
4. **§5.5 sin completar**: composición de iconos para carpetas mezcladas e icono de carpeta vacía; resaltado de código en la vista previa; recorrer las páginas del PDF; pausar y volumen en la reproducción.
5. **Mediciones que faltan**: bloqueo de la interfaz al desplazarse por miles de miniaturas, memoria real con la caché llena, caché fría en todos los tipos (video medido: ~0,2 s por video en frío desde el disco mecánico).
6. El auxiliar no es un entorno restringido (permisos del usuario) y en Windows no tiene límite de memoria.
7. Siguen abiertos los puntos de los informes de las [Fase 1](INFORME_FASE_1.md) y [Fase 2](INFORME_FASE_2.md).

## F. Siguiente fase
Fase 4 — Herramientas (renombrado masivo, organizador, transferencias con verificación, duplicados, imágenes similares, etiquetas, archivos comprimidos y búsqueda en contenido). Plan y criterios en [`PHASE_PLAN.md`](PHASE_PLAN.md).
