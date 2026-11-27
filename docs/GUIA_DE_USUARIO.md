# Guía de usuario

Explorador Ultra es un administrador de archivos para Linux, Windows y macOS. Esta guía cuenta lo esencial; los atajos se pueden cambiar en **Edición → Preferencias… → Atajos** (Ctrl+,). En macOS, donde aquí pone Ctrl es ⌘.

## La ventana
- **Árbol** (izquierda): Favoritos (carpeta personal, Escritorio, Documentos…) y Unidades. Un clic abre la carpeta y la despliega; las flechas ← → despliegan y recogen. Pulsar el rótulo **Unidades** muestra todas las unidades con su espacio libre.
- **Agregar a Favoritos**: clic derecho sobre una carpeta (en la lista o en el árbol) → «Agregar a Favoritos»; con nada seleccionado, «Agregar esta carpeta a Favoritos». También se puede arrastrar una o varias carpetas sobre el rótulo **Favoritos**. Se quitan con clic derecho → «Quitar de Favoritos» (la carpeta no se borra; los favoritos de fábrica se quedan). Se recuerdan al cerrar; si su disco no está conectado, se ven en gris como «No disponible» y vuelven al conectarlo.
- **Lista** (centro), en pestañas. Tres vistas: **Detalles** (Ctrl+1), **Miniaturas** (Ctrl+2) y **Columnas** (Ctrl+3). En Miniaturas, Ctrl + rueda (o Ctrl++ y Ctrl+-) cambia el tamaño.
- **Vista previa** (derecha, Alt+P): datos del archivo y su contenido (imágenes, PDF, texto y código, documentos de oficina, hojas CSV, video y audio con sonido).
- **Barra de ruta** (arriba): cada tramo lleva a esa carpeta; Ctrl+L o Alt+D para escribir una ruta.
- **Barra de estado** (abajo): cuántos elementos hay y, al seleccionar, cuántos y cuánto pesan (las carpetas se miden en segundo plano).

## Moverse
| Qué | Tecla |
|---|---|
| Abrir lo seleccionado | Intro o doble clic |
| Subir un nivel | Retroceso, ← en Detalles, o Alt+↑ |
| Atrás / Adelante | Alt+← / Alt+→ |
| Volver a leer la carpeta | F5 o Ctrl+R |
| Mostrar u ocultar los ocultos | Ctrl+H |
| Pestaña nueva / cerrar / reabrir la cerrada | Ctrl+T / Ctrl+W / Ctrl+Mayús+T |
| Duplicar la pestaña | Ctrl+Mayús+K |
| Pestaña siguiente / anterior | Ctrl+Tab / Ctrl+Mayús+Tab |
| Doble panel / cambiar de panel | F3 / F6 |
| Copiar / mover al otro panel | Mayús+F5 / Mayús+F6 |

Clic derecho en una pestaña: fijarla (va delante y no se cierra con la ✕), bloquearla (lo que se abra desde ella va a otra pestaña), duplicarla o cerrar las demás. Al cerrar la aplicación se guardan las pestañas, sus vistas y el doble panel.

## Archivos
| Qué | Tecla |
|---|---|
| Copiar / cortar / pegar | Ctrl+C / Ctrl+X / Ctrl+V |
| Enviar a la papelera | Supr |
| Eliminar para siempre (pide confirmación) | Mayús+Supr |
| Deshacer la última operación | Ctrl+Z |
| Renombrar / renombrar varios | F2 / Mayús+F2 (o F2 con varios seleccionados) |
| Carpeta nueva | Ctrl+Mayús+N |
| Seleccionar todo | Ctrl+A |
| Imprimir | Ctrl+P |

Lo mismo funciona en el **árbol** sobre la carpeta elegida en él (Ctrl+C, Ctrl+V; cortar y borrar solo en subcarpetas, nunca en un favorito o una unidad). También se puede **arrastrar y soltar**: dentro de la misma unidad mueve y entre unidades copia (Mayús obliga a mover, Ctrl a copiar).

Todo lo que cambia archivos queda en un **historial**: Ctrl+Z lo deshace aunque sea una operación de cientos de archivos, y si la aplicación se cierra a mitad de una copia, al volver a abrirla ofrece reanudarla. Las copias grandes se ven en **Transferencias** (Ctrl+J): pausar, cancelar, reintentar y, si se quiere, comprobar cada copia con SHA-256. Si al copiar ya existe algo con el mismo nombre, se pregunta qué hacer (reemplazar va a la papelera, mantener los dos, omitir si son idénticos…).

El **menú contextual** (clic derecho en la lista) reúne lo anterior y además: abrir, extraer y comprimir (ZIP, 7z, TAR.GZ; con varios comprimidos seleccionados se extraen todos de una vez, en su misma carpeta o cada uno en una carpeta nueva), etiquetas de color, Imprimir, Organizar esta carpeta, Buscar duplicados, Comparar y sincronizar, **Ordenar por** (nombre, tamaño, fecha o tipo, ascendente o descendente; también en el menú Ver) y la vista.

## Buscar
- **Filtro** (Ctrl+I): deja a la vista solo lo que cumple, a cada tecla, en la carpeta abierta.
- **Buscador** (Ctrl+F): busca en la carpeta y sus subcarpetas, en toda la unidad o en todas (botón **Opciones**). Admite comodines (`*.pdf`), `ext:`, `size:>5MB`, fechas, `OR`, `NOT`, expresiones regulares y buscar dentro de los documentos. Todo en [`SEARCH_SYNTAX.md`](SEARCH_SYNTAX.md).
- **Índice** (Ver → Índice de búsqueda…): opcional; anota los nombres de las unidades elegidas para que buscar en una unidad entera tarde décimas de segundo. En Windows, el **servicio opcional** de la misma ventana evita recorrer el disco al arrancar (pide permiso de administrador; se puede quitar cuando se quiera).
- **Índice de contenido** (Ver → Índice de contenido…): opcional; guarda el texto de los documentos de las carpetas elegidas para buscar dentro de ellos al instante.

## Herramientas
- **Renombrar varios** (Mayús+F2): reglas encadenadas (mayúsculas, sustituir, contador, fecha de la foto, datos de la música…) con vista previa; se deshace de una vez.
- **Organizar esta carpeta**: en subcarpetas por tipo, fecha, tamaño o datos; siempre con simulación antes y se deshace todo junto. Quita las carpetas que queden vacías.
- **Buscar duplicados** e **Imágenes parecidas**: cuánto ocupan de más; lo que se quite va a la papelera.
- **Comparar y sincronizar carpetas**: compara por nombre, tamaño, fecha o contenido; sincroniza en un sentido o en los dos. Siempre hay que **Simular** antes. Desde la segunda vez reconoce lo que se **borró** en un lado y pregunta si quitarlo también del otro, recuperarlo o dejarlo. Lo reemplazado o quitado va a la papelera propia y todo se deshace junto.
- **Etiquetas**: colores para marcar archivos; se ven en la columna Etiquetas y se buscan con `tag:`.
- **Papelera** (Edición → Papelera): restaurar o eliminar para siempre.

## Personalizar
**Edición → Preferencias…** (Ctrl+,): tema del sistema, claro, oscuro o personalizado (colores de fondo, texto, selección, acento y paneles), tamaño de letra y de iconos, atajos (avisa si una combinación ya está en uso), y exportar, importar o restaurar toda la configuración.

## Si algo falla
- Lo que hace la aplicación queda en `registros/` dentro de su carpeta de datos (en Linux `~/.local/share/ExploradorUltra/explorador-ultra/`, en Windows `%APPDATA%\ExploradorUltra\explorador-ultra\`, en macOS `~/Library/Application Support/ExploradorUltra/explorador-ultra/`). Si se cierra de golpe, `registros/fallos.log` dice dónde; adjúntalo al avisar.
- Avisos de seguridad al instalar (SmartScreen, Gatekeeper) y el permiso «Acceso total al disco» de macOS: [`DISTRIBUCION.md`](DISTRIBUCION.md).
