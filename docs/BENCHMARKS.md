# Mediciones

Solo valores medidos. Herramienta: `bench/eu-bench` (código en el repositorio) e `hyperfine`.

## 2026-11-11 — Línea base de la Fase 0 (Linux)

**Equipo:** AMD Ryzen 7 2700X (8 núcleos / 16 hilos), 60 GB de RAM, SSD SATA Samsung 860 EVO 1 TB, ext4, Ubuntu 26.04, kernel 7.0.0-43, GCC 15.2, Qt 6.12.0, compilación `RelWithDebInfo`.
**Atención:** es más potente que el equipo de referencia de §4 (4 núcleos / 8 hilos, 8 GB). Estas cifras no valen como cumplimiento de las metas en el equipo de referencia.
**Datos:** carpeta temporal con 100 000 archivos vacíos (nombres con números y con caracteres no ASCII), caché de disco caliente.

### Listar 100 000 archivos con tamaño, fecha y tipo (3 pasadas)
| Método | Total (ms) | Primeros 200 (ms) |
|---|---|---|
| POSIX `readdir` + `fstatat` | 197 – 202 | 0,9 – 1,2 |
| `std::filesystem::directory_iterator` | 437 – 453 | 1,2 – 1,3 |
| `QDirIterator` + `QFileInfo` | 754 – 784 | 1,8 |
| `QDir::entryInfoList` | 786 – 791 | 90 (no es progresivo) |

Herramientas nativas sobre la misma carpeta (`hyperfine`, 10 ejecuciones, media ± desviación):
| Orden | Tiempo |
|---|---|
| `find -printf '%s %T@ %f\n'` | 284 ± 4 ms |
| `gio list -l -h` (GIO, la misma biblioteca que usa Archivos de GNOME) | 647 ± 11 ms |
| `ls -la --sort=none` | 747 ± 13 ms |

### Orden y filtrado en memoria (100 000 nombres, un hilo)
| Operación | Tiempo |
|---|---|
| Orden natural (`eu::core::naturalCompare` + `std::sort`) | 247 ms |
| Filtro por subcadena sin distinguir mayúsculas, "foto" (10 000 coincidencias) | 15,8 ms |
| Filtro "9999" (19 coincidencias) | 16,3 ms |
| Filtro sin coincidencias | 16,9 ms |
| Filtro "a" (todo coincide) | 4,3 ms |

### Copia de un archivo de 4 GB, SSD → mismo SSD, con `fsync` final (3 pasadas)
| Método | MB/s |
|---|---|
| `cp` + `sync` (herramienta nativa) | 373 – 418 |
| Bucle `read`/`write` de 1 MiB + `fsync` | 405 – 443 |
| `std::filesystem::copy_file` + `fsync` | 408 – 435 |
| `QFile::copy` + `fsync` | 415 – 433 |

El archivo de origen estaba en la caché de RAM, así que estas cifras miden sobre todo la escritura. Origen y destino en el mismo disco.

## Fase 2 — Filtrado local (2026-11-12)
Equipo de desarrollo (Ubuntu 26.04, SSD), compilación `release`, medido por las propias pruebas (`test_search_query` y `test_ui_directory_model`), que imprimen la cifra en cada ejecución.

| Medida | Resultado | Meta |
|---|---|---|
| Evaluar una consulta con comodines, `OR`, `ext:` y `NOT` contra 100 000 nombres de ~45 caracteres | ~15 ms | < 50 ms |
| Aplicar un filtro a una carpeta de 100 000 elementos en el modelo de la vista (10 resultados) | 5 ms | < 50 ms por tecla |
| Ídem con 3691 resultados | 7 ms | < 50 ms por tecla |
| Quitar el filtro (vuelven los 100 000) | < 1 ms | < 50 ms por tecla |

Lo que **no** incluyen estas cifras: el tiempo que tarda la vista en repintar tras el cambio, que no se ha medido.

### Búsqueda en subcarpetas (Fase 2, paso 3)
Sin índice: recorre el disco cada vez. Cifras de este equipo (SSD NVMe, caché caliente), 12-nov.

| Qué | Resultado | Meta |
|---|---|---|
| Buscar `ext:pdf` en la carpeta personal (12 418 carpetas) | terminó en menos de 1,5 s (observado en la ventana, no cronometrado con precisión) | — |
| Cancelar una búsqueda en pausa (`test_search_searcher`) | < 500 ms (la prueba falla si tarda más) | < 500 ms |

No medido: caché fría, discos mecánicos, unidades de red, bloqueo del hilo de interfaz con cientos de miles de resultados.

### Índice de búsqueda (Fase 2, paso 5)
`eu-bench index <carpeta> <archivo.db> [consultas…]`, compilación release, este equipo, caché caliente, 13-nov. Las consultas se miden dentro del proceso (sin interfaz): tiempo total y tiempo hasta el primer resultado.

| Qué | `/media/merwin/RESPALDO` (199 512 entradas, 135 517 carpetas) | `/` (572 947 entradas, 70 501 carpetas) |
|---|---|---|
| Construir el índice | 3,3 s · base de 29 MB | 10,4 s · base de 42,5 MB |
| Ponerlo al día sin cambios (recorrido completo) | 2,6 s | 3,0 s |
| `walter` | 20 ms (primero a 1,5 ms) | 60 ms (primero a 4 ms) |
| `walter riso*` | 32 ms (primero a 4 ms) · 102 resultados | — |
| `*.pdf` | 40 ms · 16 186 resultados | 70 ms · 4 363 resultados |
| `ext:jpg size:>5MB` | 30 ms (primero a 20 ms) | 65 ms (primero a 3 ms) |
| `size:>1GB` (sin pista de nombre: lee todas las filas) | 160 ms | 380 ms (primero a 0,3 ms) |
| `regex:/^IMG_\d+/` (sin pista de nombre) | 214 ms | 476 ms (0 resultados) |
| Algo que no existe (`zzzznoexiste`) | 20 ms | 62 ms |

Lectura: con una pista de nombre (palabra, comodín o extensión) la base descarta las filas y la consulta tarda decenas de milisegundos. Sin pista (solo tamaño, fecha, expresión regular o letras no ASCII) hay que leer todas las filas: unos 0,8 µs por fila.

**Meta de SPEC §4 (1 000 000 de entradas, primeros resultados < 200 ms): no medida con un millón.** La unidad mayor medida tiene 573 000. Extrapolando, las consultas con pista quedarían holgadamente por debajo; una consulta sin pista y sin ningún resultado tardaría unos 0,8 s en terminar. Falta medirlo de verdad, y también: caché fría, discos mecánicos, tiempo visto en la ventana, Windows y macOS.

### No medido todavía (pendiente)
- Tiempo de apertura real de una carpeta en Archivos de GNOME (no hay forma fiable de automatizarlo; `gio list` es una aproximación de su motor, no de su interfaz).
- Caché fría, HDD, USB y unidades de red.
- Memoria por elemento: la cifra de RSS que imprime `eu-bench listing` no es fiable porque las cuatro pruebas comparten proceso y el asignador reutiliza memoria; se medirá con el modelo real en la Fase 1.
- Windows (VM y CI) y macOS (CI).
- Bloqueo del hilo de interfaz, miniaturas y cancelación: requieren código de las Fases 1 a 3.
