# Plan de las fases 1 a 6

Cada fase se desarrolla en su rama (`fase-N/...`), se fusiona con la CI en verde y cierra con `STATUS.md` actualizado. Una función solo es "Terminada" si cumple §26.

## Fase 1 — Explorador funcional
**Módulos:** `core` (errores, rutas, registro), `tasks` (versión mínima), `fs`, `ops`, `journal`, `trash`, `settings` (mínimo), `ui`, `app`.
**Orden de trabajo:**
1. `core`: tipo `Error`/`Result`, validación de nombres por plataforma, registro rotativo, formato de tamaños (SI y binario).
2. `tasks`: pools por categoría, prioridades y cancelación (lo justo para listar y copiar; se completa en la Fase 2).
3. `fs`: `Location`, listado nativo progresivo, vigilancia de cambios, detección de unidades en caliente.
4. `ui`: ventana, árbol, vista detallada virtualizada, migas de pan, navegación e historial, pestañas básicas, tamaño de carpetas.
5. `journal` + `ops` + `trash`: crear, copiar, mover, renombrar, eliminar, restaurar, deshacer y recuperación tras cierre inesperado.
6. Atajos principales, manejo de errores visible, arrastrar y soltar.

**Criterios de aceptación:**
- Carpeta de 100 000 archivos: primeros elementos < 100 ms y listado completo ordenado < 1 s en este equipo; bloqueo de interfaz < 50 ms (medido).
- Orden natural correcto; tamaños en SI con opción binaria; tamaño de carpetas progresivo y cancelable.
- Copiar, mover (incluido entre volúmenes), renombrar y eliminar a la papelera propia, todo reversible con Deshacer y persistente tras reiniciar.
- Matar la aplicación a mitad de un mover no pierde ni duplica datos; al reabrir se ofrece reanudar, revertir o descartar.
- La papelera se ubica en el mismo volumen (verificado por dispositivo) y restaura aunque se borre la base de datos.
- Conectar y desconectar una USB actualiza el árbol sin congelar; rutas no disponibles no bloquean.
- Pruebas de enlaces simbólicos, ciclos, permisos, rutas largas y Unicode en verde en las tres plataformas.
- Verificado a mano en Ubuntu (Wayland y X11) y en la VM de Windows.

## Fase 2 — Búsqueda y rendimiento
**Módulos:** `tasks` (completo, concurrencia adaptativa por dispositivo), `search`, `index`.
**Criterios de aceptación:**
- Gramática documentada en `docs/SEARCH_SYNTAX.md`; el analizador pasa pruebas de comodines, `AND`/`OR`/`NOT`, paréntesis, todos los filtros de §6.1 y `regex:`.
- Resultados progresivos como ubicación virtual, con pausa, cancelación (< 500 ms) y contadores.
- Filtrado local de 100 000 elementos < 50 ms por tecla en este equipo.
- Índice SQLite opcional con asistente de primer arranque; actualización por vigilancia de cambios; con 1 000 000 de entradas, primeros resultados < 200 ms (medido).
- Resultados "del índice" distinguibles y verificables; aviso y guía si el límite de inotify no alcanza.
- NFC/NFD: una búsqueda encuentra el mismo nombre en ambas normalizaciones.

## Fase 3 — Miniaturas, previsualización y metadatos
**Módulos:** `thumbs`, `preview`, `meta`.
**Criterios de aceptación:**
- Vista de miniaturas para imágenes (incluidos SVG y RAW, con orientación EXIF), PDF, texto, video (fotograma del segundo 3), audio (carátula) y carpetas.
- Ofimática con LibreOffice si existe (proceso aislado con tiempo límite) y alternativa si no.
- Caché en disco por ruta + tamaño + fecha, con límite de RAM configurable (256 MB por defecto); desplazarse rápido cancela lo obsoleto.
- Reproducción en miniatura de video y audio, con límite de decodificadores.
- Panel de previsualización y metadatos por tipo; archivos dañados o maliciosos no cuelgan ni cierran la aplicación (decodificación en proceso auxiliar).

## Fase 4 — Herramientas
**Módulos:** `rename`, `organize`, `ops` (cola de transferencias completa), `dupes`, `similar`, `tags`, `archive`, `search` (contenido, FTS5).
**Criterios de aceptación:**
- Renombrado masivo con vista previa, detección de conflictos y ciclos, por metadatos, y deshacer en bloque.
- Organizador con simulación obligatoria y deshacer en bloque.
- Cola de transferencias con pausa, reanudación, verificación SHA-256, todas las políticas de colisión; copia ≥ 90 % de la velocidad nativa (medido).
- Duplicados por etapas (tamaño → hash parcial → hash completo → byte a byte opcional), distinguiendo enlaces físicos; nunca borra solo.
- Imágenes similares con umbral ajustable y comparación lado a lado.
- Etiquetas en base local que sobreviven a mover y renombrar; huérfanas revisables.
- Comprimidos: navegar, extraer, crear ZIP/7z/TAR/TAR.GZ; RAR e ISO solo lectura; pruebas de zip-slip y bombas en verde.
- Búsqueda en contenido en los formatos de §6.5, con fragmento resaltado y límites por archivo.

## Fase 5 — Funciones profesionales
**Módulos:** `ui` (Miller, doble panel, personalización), `sync`, `settings` (completo), `print`, `indexsvc-win`.
**Criterios de aceptación:**
- Columnas Miller navegables por teclado; doble panel con arrastre entre paneles.
- Comparación y sincronización uni y bidireccional con simulación obligatoria; sin política explícita no propaga borrados.
- Sesión completa restaurada al reabrir; pestañas fijadas y bloqueadas.
- Temas claro, oscuro y personalizado aplicados al instante; atajos personalizables con detección de conflictos; exportar e importar configuración.
- Impresión con vista previa de imagen, PDF, texto y código.
- Servicio de Windows opcional: se instala y desinstala con elevación explícita, solo lectura de MFT/USN, y la aplicación funciona igual sin él. Verificado en la VM.

## Fase 6 — Optimización y distribución
**Criterios de aceptación:**
- Todas las metas de §4 medidas y anotadas; las que no se cumplan, documentadas con causa.
- Pruebas integrales y sanitizers en verde; sin regresiones conocidas.
- Artefactos generados por la CI en cada etiqueta: `.deb` y AppImage (probados en Ubuntu), instalador de Windows (probado en la VM) y `.dmg` (solo compilado; sin verificación manual).
- `README.md` con compilación reproducible, `THIRD_PARTY_LICENSES.md`, guía de usuario y guías de SmartScreen, Gatekeeper y "Acceso total al disco".

## Fuera de alcance (documentado, sin implementar)
Monitor de rendimiento integrado (CPU, RAM, disco, archivos/s, MB/s, trabajadores).
