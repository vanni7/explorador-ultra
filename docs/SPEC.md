# PROYECTO: EXPLORADOR ULTRA
## Especificación de desarrollo para agente — versión 2.0

> Este documento es la especificación oficial y única fuente de verdad del proyecto. Guárdalo en el repositorio como `docs/SPEC.md` y respétalo en cada sesión. Si algo aquí entra en conflicto con una instrucción posterior del usuario, prevalece la instrucción del usuario y debes registrarla en `docs/DECISIONS.md`.

---

## 0. ROL, AUTORIZACIÓN Y FORMA DE TRABAJO

### 0.1 Rol
Actúas como un equipo de ingeniería de software especializado en:

- aplicaciones de escritorio multiplataforma con C++20 y Qt 6;
- programación concurrente;
- optimización de E/S y sistemas de archivos (NTFS, APFS, ext4, btrfs, exFAT, FAT32, SMB/NFS);
- procesamiento multimedia y miniaturas;
- seguridad e integridad de datos;
- UX/UI de escritorio;
- pruebas automatizadas, CI y empaquetado.

Tu objetivo es construir **Explorador Ultra**: un administrador de archivos nativo, real, rápido, robusto y distribuible para Windows, macOS y Linux (prioridad Ubuntu).

### 0.2 Autorización
Trabajas en el equipo Ubuntu del usuario mediante Claude Code. **Tienes autorización total** para instalar, configurar y actualizar todo lo necesario para desarrollar, compilar, probar y empaquetar la aplicación. Esto incluye:

- usar `sudo`, `apt`, `snap`, `pip`, `pipx`, `npm`, `vcpkg`, `aqtinstall`, compiladores, CMake, Ninja, ccache, clang-tidy, clang-format, depuradores, sanitizers y profilers;
- instalar Qt 6 y las bibliotecas del proyecto;
- instalar VirtualBox y crear, configurar y administrar máquinas virtuales **Windows 10/11** (ISO de evaluación oficial de Microsoft) para compilar y probar;
- crear y administrar el repositorio Git y los flujos de GitHub Actions con la autenticación de `gh` que el usuario proporcione. Si no hay autenticación disponible, pídela una sola vez y continúa con el resto del trabajo mientras tanto.

**macOS no se virtualiza**: la licencia de Apple lo prohíbe en hardware que no es de Apple. macOS se compila y prueba exclusivamente con runners `macos-*` de GitHub Actions (repositorio público).

Aun con autorización total, mantén estas salvaguardas:

1. Las pruebas que operan sobre archivos se ejecutan **solo** en directorios temporales creados para ello, nunca sobre datos reales del usuario.
2. No borres ni modifiques archivos del usuario fuera del directorio del proyecto, de las VM y de las rutas de herramientas que instales.
3. Registra en `docs/ENVIRONMENT.md` cada herramienta instalada, su versión y el comando usado, para que el entorno sea reproducible.

### 0.3 Continuidad entre sesiones
El proyecto abarcará muchas sesiones. Mantén en el repositorio:

| Archivo | Contenido |
|---|---|
| `CLAUDE.md` | Resumen operativo: cómo compilar, probar, convenciones y comandos clave. Léelo al inicio de cada sesión. |
| `docs/SPEC.md` | Este documento. |
| `docs/ARCHITECTURE.md` | Módulos, dependencias entre módulos, modelo de hilos y flujo de datos. |
| `docs/DECISIONS.md` | Registro de decisiones (formato ADR): contexto, decisión, alternativas y consecuencias. |
| `docs/STATUS.md` | Estado de cada función: **Terminada / Parcial / Pendiente / Bloqueada**, con la plataforma donde se verificó. |
| `docs/DEPENDENCIES.md` | Bibliotecas, versión, licencia, uso y forma de enlace. |
| `docs/ENVIRONMENT.md` | Entorno de desarrollo reproducible. |
| `docs/BENCHMARKS.md` | Solo mediciones reales: fecha, equipo, método y resultado. |

Al inicio de cada sesión lee `CLAUDE.md` y `docs/STATUS.md`. Al final de cada sesión actualízalos.

---

## 1. DECISIONES YA TOMADAS (no reabrir sin causa técnica documentada)

| Tema | Decisión |
|---|---|
| Licencia | **GPL-3.0-or-later**. Distribución gratuita y código abierto. Toda dependencia debe ser compatible con GPL-3. |
| Lenguaje y UI | **C++20 + Qt 6** (la versión LTS más reciente disponible; verifícala). Widgets clásicos o Qt Quick según la evaluación de la Fase 0, justificada en un ADR. |
| Construcción | CMake + Ninja. Dependencias mediante vcpkg o paquetes del sistema, documentadas. |
| Plataformas mínimas | Windows 10 22H2 y Windows 11; macOS 13 o posterior (Intel y Apple Silicon, universal binary si es viable); Ubuntu 22.04 LTS y 24.04 LTS con **X11 y Wayland**. |
| Pruebas | Ubuntu: local. Windows: VM de VirtualBox + GitHub Actions. macOS: solo GitHub Actions. |
| Indexación | **Opcional**. En el primer arranque, un asistente propone qué unidades indexar; el usuario puede aceptar, elegir unidades, posponer o rechazar. La indexación corre en segundo plano, con prioridad baja, y se puede pausar. |
| Búsqueda acelerada en Windows | **Servicio opcional** con privilegios que lee la MFT y el USN Journal de NTFS. Se instala solo con consentimiento explícito y se puede desinstalar. La aplicación funciona plenamente sin él, usando un recorrido normal y vigilancia de cambios. |
| Papelera | **Papelera propia por volumen**, llamada `PapeleraExploradorUltra`. No se usa la papelera del sistema (detalle en §10.2). |
| Miniaturas de ofimática | **LibreOffice opcional**: si está instalado se usa en modo headless y aislado. Si no, se usa la miniatura incrustada en el archivo (cuando exista), un extracto de texto o un icono. |
| Firma de código | **Sin firma**. Se documenta cómo autorizar la app ante SmartScreen y Gatekeeper. El pipeline se diseña para añadir firma en el futuro sin rediseño. |
| Idioma | Interfaz inicial en español; arquitectura preparada para traducciones (Qt Linguist). |
| Funciones extra incluidas | Organizador automático, búsqueda en contenido y detección de imágenes similares. |
| Funciones futuras (documentar, no implementar) | Monitor de rendimiento integrado. |
| Privacidad | Todo funciona localmente. Nunca se envía información a servidores externos. |

---

## 2. PRIORIDADES (en orden)

1. Seguridad e integridad de los archivos.
2. Funcionamiento real de cada herramienta.
3. Rendimiento y capacidad de respuesta.
4. Compatibilidad multiplataforma.
5. Facilidad de uso.
6. Diseño visual moderno.
7. Extensibilidad y mantenibilidad.

Nunca sacrifiques la integridad de los datos por velocidad aparente.

**Mantenibilidad:** el usuario programa en PHP, Python y VB.NET, no en C++. El código debe ser especialmente legible: nombres descriptivos, comentarios que expliquen el *porqué*, poca "magia" de plantillas, propiedad de memoria explícita (RAII y punteros inteligentes) y documentación de cada módulo en español.

---

## 3. ARQUITECTURA

### 3.1 Módulos
Cada módulo es una biblioteca CMake independiente con interfaz pública mínima y pruebas propias. Ningún módulo de núcleo depende de la interfaz gráfica.

1. `core` – tipos comunes, rutas, errores, registro (logging) y utilidades Unicode.
2. `tasks` – planificador de tareas concurrentes: pools por categoría, prioridades y cancelación.
3. `fs` – navegación, listado, vigilancia de cambios y detección de unidades.
4. `ops` – operaciones de archivos, transferencias, colisiones y verificación de integridad.
5. `journal` – historial persistente de operaciones, deshacer y recuperación.
6. `trash` – papelera propia por volumen.
7. `search` – motor de búsqueda, sintaxis de consultas y búsqueda en contenido.
8. `index` – índice local (SQLite) y su actualización incremental.
9. `indexsvc-win` – servicio opcional de Windows (MFT/USN), en un proceso separado.
10. `thumbs` – generación y caché de miniaturas.
11. `preview` – previsualización segura.
12. `meta` – metadatos (EXIF, ID3/tags, multimedia, documentos).
13. `rename` – renombrado individual y masivo.
14. `organize` – organizador automático.
15. `dupes` – duplicados exactos.
16. `similar` – imágenes similares (hash perceptual).
17. `archive` – archivos comprimidos y navegación virtual.
18. `sync` – comparación y sincronización.
19. `tags` – etiquetas y colores.
20. `settings` – configuración, sesiones, temas y atajos.
21. `print` – impresión.
22. `ui` – interfaz gráfica.
23. `app` – arranque, integración y empaquetado.

### 3.2 Sistema de archivos virtual
Define una abstracción de ubicación (`Location`) que unifique: carpetas locales, unidades de red, el contenido de archivos comprimidos, los resultados de búsqueda y la papelera. Las vistas, la previsualización y las operaciones trabajan sobre esta abstracción, de modo que navegar dentro de un ZIP o de unos resultados de búsqueda sea igual que navegar una carpeta.

### 3.3 Modelo de hilos
- El hilo de la interfaz **nunca** realiza E/S de disco ni cálculos costosos.
- Usa pools separados y acotados por categoría: listado, E/S de transferencias, hashing, decodificación multimedia, miniaturas, indexación, metadatos y compresión.
- Prioridades: (1) interacción del usuario, (2) navegación visible, (3) previsualización solicitada, (4) transferencias activas, (5) miniaturas visibles, (6) trabajos secundarios.
- Cada tarea admite cancelación cooperativa y reporta progreso.
- La concurrencia por dispositivo es adaptativa: se detecta SSD, HDD, USB o red y se limita en consecuencia (por ejemplo, un solo flujo secuencial por HDD). No se asigna un núcleo exclusivo a cada disco.
- Todos los límites de concurrencia son configurables.

### 3.4 Bibliotecas candidatas
Evalúalas en la Fase 0 y registra la elección en `docs/DEPENDENCIES.md`. Todas deben ser compatibles con GPL-3 y gratuitas.

- Archivos comprimidos: **libarchive** (lectura de RAR y RAR5; lectura y escritura de ZIP, 7z y TAR).
- Multimedia: **FFmpeg** (directamente o mediante Qt Multimedia).
- PDF: Qt PDF o Poppler.
- Metadatos: **Exiv2** (EXIF/XMP/IPTC) y **TagLib** (ID3 y otras etiquetas de audio).
- Imágenes RAW: **LibRaw**.
- SVG: Qt SVG.
- Base de datos local: **SQLite** (modo WAL).
- Hashes: SHA-256, SHA-1 y MD5 mediante Qt u OpenSSL; CRC32 mediante zlib; hash parcial rápido con xxHash.
- Pruebas: Qt Test o GoogleTest.

No añadas dependencias sin justificarlas en un ADR.

---

## 4. METAS DE RENDIMIENTO VERIFICABLES

**Equipo de referencia:** CPU de 4 núcleos / 8 hilos, 8 GB de RAM, SSD SATA y, como segundo disco, un HDD o una USB 3.

Estas metas son iniciales. En la Fase 0 crea las herramientas de medición, mide una línea base contra el explorador nativo de cada sistema y ajusta las metas en un ADR. **Nunca reportes un valor que no se haya medido.**

| Escenario | Meta inicial |
|---|---|
| Abrir una carpeta con 100 000 archivos (SSD, caché caliente) | Primeros elementos visibles en < 300 ms; listado completo en < 3 s |
| Bloqueo máximo del hilo de interfaz | < 50 ms (objetivo: 16 ms) |
| Filtrado local sobre 100 000 elementos | < 100 ms por tecla |
| Búsqueda por nombre con índice de 1 000 000 de entradas | Primeros resultados en < 200 ms |
| Memoria con 100 000 elementos en una pestaña, sin miniaturas | < 300 MB |
| Caché de miniaturas en RAM | Configurable; predeterminado 256 MB |
| Copia grande sin verificación | ≥ 90 % de la velocidad de la herramienta nativa en el mismo equipo |
| Cancelación de cualquier tarea | Efectiva en < 500 ms |

---

## 5. INTERFAZ

### 5.1 Identidad visual
- Diseña un icono original en SVG que transmita exploración, rapidez, organización y tecnología, y que sea legible a 16 px.
- Genera los recursos para cada plataforma: `.ico` (Windows), `.icns` (macOS) y PNG en los tamaños freedesktop (Linux).
- El diseño debe ser moderno y coherente, con animaciones discretas, soporte de DPI alto y escalado fraccionario, y tipografía legible.

### 5.2 Personalización
- Temas claro, oscuro y personalizado.
- Colores configurables: fondo, texto, selección, acento y paneles.
- Tamaño de iconos, miniaturas y fuente.
- Los cambios se aplican al instante y se conservan entre sesiones.
- Opción para restaurar los valores predeterminados.

### 5.3 Ventana principal
Consta de una barra superior y tres paneles ajustables y ocultables: árbol a la izquierda, exploración en el centro y previsualización a la derecha.

**Barra superior:**
- botones Atrás, Adelante y Subir;
- ruta editable con autocompletado y migas de pan clicables;
- buscador global con selector de modalidad;
- botones de vista y acceso a filtros y herramientas;
- indicador de operaciones activas.

Las herramientas de la barra se pueden reorganizar, mostrar u ocultar.

**Panel izquierdo (árbol):**
- unidades internas y externas, USB, particiones, unidades de red, favoritos y accesos rápidos;
- detección de conexión y desconexión de unidades en caliente;
- solo muestra lo accesible y maneja los errores de lectura;
- las unidades de red usan tiempos de espera y nunca congelan la interfaz.

**Panel central:**
- tres vistas: detallada, miniaturas y columnas Miller;
- al cambiar de vista se conservan la ruta y la selección cuando es posible;
- **modo de doble panel**: un botón divide el centro en dos exploradores independientes, cada uno con su ruta, vista e historial;
- se puede arrastrar entre ambos paneles, y desde ahí se lanzan la comparación y la sincronización.

**Panel derecho:** metadatos arriba y previsualización abajo, adaptados al tipo de archivo.

### 5.4 Vista detallada
- Columnas: Tipo (icono), Nombre, Tamaño y Fecha de modificación, ordenables. Columnas adicionales opcionales: fecha de creación, tipo y etiquetas.
- Ordenamiento natural (`archivo2` antes de `archivo10`).
- Tamaños en B, KB, MB, GB o TB. Unidades **decimales (SI) por defecto**, con opción de unidades **binarias (KiB, MiB…)**; la convención usada se muestra en la interfaz.
- **Tamaño de carpetas:** cálculo en segundo plano con actualización progresiva, indicador visual, caché, invalidación ante cambios, cancelación y motivo visible cuando no se puede calcular. Nunca bloquea la interfaz.
- Lista virtualizada: no se crean widgets por elemento.

### 5.5 Vista de miniaturas
- **Imágenes** (incluye SVG y RAW): imagen completa, relación de aspecto conservada, centrada, sin recortes y respetando la orientación EXIF.
- **Documentos** (TXT, PDF, DOCX, ODT, XLSX, ODS, PPTX, ODP, RTF, Markdown y código): primera página o vista representativa. Para ofimática, LibreOffice si está disponible (en proceso aislado y con tiempo límite); si no, miniatura incrustada, extracto de texto o icono.
- **Video:** fotograma del segundo 3, o uno disponible si el video es más corto. Un botón ▶ reproduce en silencio dentro de la miniatura y se detiene cuando el puntero sale. Función activable y desactivable, con control por teclado. Hay un límite global de decodificadores simultáneos (predeterminado: 1 en reproducción y N en extracción, según núcleos).
- **Audio:** icono o carátula incrustada como fondo, botón ▶ y duración visible. Se detiene cuando el puntero sale. Control por teclado.
- **Carpetas:** si el contenido es de un solo formato, hasta cuatro miniaturas de los primeros archivos; si es mixto, una composición de iconos; si está vacía, un icono de carpeta vacía.
- **Optimización:** generación asíncrona, caché persistente en disco (por ruta + tamaño + fecha de modificación), carga diferida, prioridad a lo visible, cancelación de solicitudes obsoletas, invalidación ante cambios y límites de memoria.

### 5.6 Columnas Miller
- Navegación horizontal tipo Finder, con columnas de ancho ajustable y desplazamiento horizontal.
- Indicadores de carpeta activa y navegación completa por teclado.
- Integrada con la previsualización.
- Manejo de rutas profundas.

### 5.7 Pestañas y sesiones
- Abrir, cerrar, duplicar, reordenar, fijar y recuperar pestañas cerradas.
- Pestaña **bloqueada**: su ruta no cambia; navegar desde ella abre una pestaña nueva.
- Al reabrir la aplicación se restauran: pestañas, rutas, pestañas fijadas, vistas, distribución de paneles, anchos de columnas y estado del doble panel.
- Las rutas inexistentes o de unidades desconectadas se muestran como no disponibles y se pueden reintentar, sin errores bloqueantes.

---

## 6. BÚSQUEDA

### 6.1 Sintaxis de consulta
Documenta la gramática formal en `docs/SEARCH_SYNTAX.md`.

**Comodines:**
- `?` representa exactamente un carácter.
- `*` representa cero o más caracteres.

Ejemplos: `fot*.jpg`, `informe_202?.pdf`, `*factura*`, `documento?.docx`.

**Distinción de mayúsculas:** desactivada por defecto, activable. Compatible con Unicode, con normalización NFC/NFD (necesaria para macOS).

**Operadores:**
- `AND`, `OR` y `NOT`. Precedencia: `NOT` > `AND` > `OR`. Se admiten paréntesis.
- Un espacio entre términos equivale a `AND`.

**Filtros:**
- `ext:pdf`
- `size:>10MB`
- `modified:2025-01..2025-06`
- `created:`
- `type:imagen`
- `tag:rojo`
- `hidden:yes`
- `content:"texto"`
- `regex:/patrón/`

**Búsquedas guardadas:** se pueden guardar y reutilizar.

### 6.2 Alcance
Carpeta actual, carpeta actual con subcarpetas, unidad seleccionada, varias unidades o todas las unidades accesibles.

### 6.3 Ejecución
- Los resultados se muestran de forma progresiva en el panel central, como una ubicación virtual.
- Indicadores visibles: progreso, número de coincidencias y ubicaciones examinadas.
- Se puede pausar y cancelar.
- Los errores y la falta de permisos se registran sin interrumpir la búsqueda.
- La búsqueda es concurrente y adaptativa por dispositivo (§3.3).

### 6.4 Índice híbrido
- Índice local opcional en SQLite, actualizado mediante vigilancia de cambios: ReadDirectoryChangesW, FSEvents o inotify/fanotify, según la plataforma.
- Si el volumen admite límites de inotify bajos, documéntalo y ofrece cómo ampliarlos.
- Los resultados del índice se marcan como **"del índice"** y se distinguen de los confirmados por recorrido directo. Existe una opción para verificar los resultados.
- **Asistente de primer arranque:** descrito en §1.
- **Servicio de Windows opcional:** proceso separado, con IPC por *named pipes*, acceso de solo lectura a la MFT y al USN Journal, instalación y desinstalación desde la configuración con elevación explícita. Si el servicio no está o falla, la aplicación recurre al método normal.

### 6.5 Búsqueda en contenido
- Busca dentro de TXT, código, Markdown, PDF, DOCX, ODT, XLSX, ODS, PPTX, ODP y RTF.
- Muestra la ruta, la página o ubicación cuando sea posible, y un fragmento con la coincidencia resaltada.
- Usa un índice de texto completo opcional (SQLite FTS5) por ubicación.
- Tiene límites de tamaño por archivo y tiempo máximo por archivo.
- La extracción de texto nunca ejecuta macros ni scripts.

### 6.6 Filtrado local
- Filtra al instante la lista activa mientras el usuario escribe, sin volver a leer el disco.
- Se distingue visualmente de la búsqueda global.

---

## 7. PREVISUALIZACIÓN Y METADATOS

### 7.1 Previsualización
- **Formatos:** imágenes, PDF, texto, Markdown (renderizado), código (con resaltado), audio, video y ofimática compatible.
- **Requisitos técnicos:** lectura asíncrona, carga progresiva, cancelación al cambiar de selección, caché, límites de memoria y lectura parcial de archivos grandes (por ejemplo, las primeras N MB de un texto).
- **Seguridad:** tolerancia a archivos dañados. Nunca se ejecutan scripts, macros ni contenido activo. Las rutinas de decodificación de riesgo se aíslan cuando sea viable.

### 7.2 Metadatos
- Nombre, tipo, tamaño, fechas, resolución, duración, códec, ID3, EXIF y otros atributos, según el formato.
- Solo se muestra lo que aplica a cada tipo de archivo.

---

## 8. OPERACIONES DE ARCHIVOS

### 8.1 Operaciones básicas
- Crear, abrir, abrir con…, copiar, cortar, pegar, mover, renombrar, eliminar (a la papelera propia), eliminar permanentemente (con confirmación reforzada), restaurar, propiedades y arrastrar y soltar (interno y con otras aplicaciones; probar en X11 y Wayland).
- Funcionan entre unidades distintas cuando hay permisos.

### 8.2 Renombrado directo
- Mediante F2, el menú contextual o un clic lento sobre el nombre ya seleccionado. El comportamiento es configurable.
- El doble clic abre el archivo; el renombrado nunca interfiere con la apertura.

### 8.3 Seguridad de las operaciones
- **Mover entre volúmenes:** copiar, verificar (si está activado), sincronizar a disco y solo después borrar el origen.
- **Escritura atómica:** escribir en un archivo temporal en el mismo destino y luego renombrar.
- **Enlaces simbólicos:** nunca se siguen al eliminar, y se previenen los ciclos al recorrer.
- **Rutas:** compatibilidad con rutas largas en Windows (prefijo `\\?\` y manifiesto *longPathAware*), y validación de nombres inválidos por plataforma (reservados de Windows, `:` en macOS, etc.).
- **Permisos:** no se solicitan privilegios de administrador, salvo para el servicio opcional de Windows.

---

## 9. HISTORIAL Y DESHACER (`journal`)
- Toda operación que modifica archivos se registra **antes** de ejecutarse (write-ahead) en una base de datos persistente: tipo, origen, destino, estado y marca de tiempo.
- Deshacer está disponible para renombrar (individual y masivo), mover, copiar (eliminando la copia), enviar a la papelera, organizar y crear archivos y carpetas.
- El historial sobrevive a reinicios. Retención configurable: 30 días o 10 000 operaciones por defecto.
- Antes de deshacer se verifica que el estado actual coincide con el esperado. Si algo cambió fuera de la aplicación, se informa y se deshace solo lo seguro.
- **Recuperación tras cierre inesperado:** al iniciar, se detectan las operaciones incompletas y se ofrece reanudarlas, revertirlas o descartarlas.
- Las acciones no reversibles (eliminación permanente, sobrescritura) se advierten **antes** de ejecutarse.

---

## 10. PAPELERA PROPIA

### 10.1 Comportamiento general
"Eliminar" envía los elementos a `PapeleraExploradorUltra` **en el mismo volumen** que el original, de modo que el envío sea solo un renombrado, rápido y atómico. Nunca se copia entre discos.

### 10.2 Ubicación por volumen
- Si se puede escribir en la raíz del volumen: `<raíz>/PapeleraExploradorUltra/`.
- Si no (por ejemplo, `/` en Linux o macOS): el directorio escribible más alto del usuario en ese mismo volumen, por ejemplo `~/PapeleraExploradorUltra/`. Se verifica que realmente esté en el mismo volumen comparando el identificador de dispositivo.
- Si no existe ninguna ubicación escribible en el volumen (solo lectura, ciertos recursos de red): se informa y solo se ofrece la eliminación permanente con confirmación reforzada.

### 10.3 Estructura y restauración
- Cada elemento guarda junto a él un archivo de metadatos: ruta original, fecha de eliminación, tamaño e identificador. Esto permite restaurar aunque la base de datos se pierda.
- La carpeta de la papelera se crea oculta según la convención de cada plataforma y se excluye del índice y de las búsquedas, salvo que se pida explícitamente.
- Una vista unificada muestra la papelera de todos los volúmenes conectados, con opciones para restaurar, eliminar permanentemente y vaciar (por volumen o todo).
- Los volúmenes desconectados aparecen como no disponibles.
- La retención y el tamaño máximo son configurables, y siempre se avisa antes de purgar.

---

## 11. RENOMBRADO MASIVO
- **Alcance:** selección, carpeta actual o recursivo, incluyendo o excluyendo carpetas.
- **Transformaciones:** MAYÚSCULAS, minúsculas, Tipo Título, Tipo oración, sustituir, eliminar caracteres, prefijo y sufijo, expresiones regulares, contadores con formato y fechas. Las reglas se pueden combinar en cadena, y se puede elegir conservar la extensión.
- **Por metadatos:** EXIF (fecha de toma, cámara…), ID3 (artista, álbum, pista, título, año; el usuario elige campos y orden) y también fechas, dimensiones y otras propiedades.
- **Seguridad:** vista previa *Nombre actual → Nombre propuesto*, con detección de conflictos, nombres inválidos, archivos protegidos, colisiones por mayúsculas y minúsculas, y renombrados en cadena o ciclo (resueltos con nombres temporales). Se registra en el historial y se puede deshacer.

---

## 12. ORGANIZADOR AUTOMÁTICO
- Sobre una carpeta elegida, crea una estructura según reglas: por **extensión o tipo**, **fecha**, **tamaño** o **metadatos**. Ejemplo predeterminado:
  - `Documentos/PDF`, `Documentos/Word`, `Documentos/Excel`
  - `Imágenes/JPG`, `Imágenes/PNG`, `Imágenes/RAW`
  - `Videos/MP4`, `Videos/MKV`
  - `Audio/MP3`, `Audio/FLAC`
- Las reglas son personalizables y se pueden guardar.
- Se puede optar por no recorrer subcarpetas.
- **Siempre** se muestra una simulación completa (árbol resultante y lista de movimientos) antes de aplicar.
- Se ejecuta como una operación del historial, reversible en bloque.

---

## 13. TRANSFERENCIAS
- **Cola de operaciones:** pausar, reanudar, cancelar, reintentar y priorizar; modo secuencial o paralelo.
- **Información visible:** velocidad, tiempo restante, progreso individual y global.
- **Concurrencia adaptativa** por dispositivo; nunca se saturan discos mecánicos.
- **Reanudación** de transferencias interrumpidas cuando sea viable (copia por bloques con punto de control).
- **Integridad:** SHA-256 por defecto; SHA-1, MD5 y CRC32 disponibles con su limitación indicada. Opciones: verificación posterior, comparación origen-destino, aviso y reintento ante errores. La verificación automática es configurable.
- **Colisiones:** sobrescribir, omitir, sobrescribir si el origen es más reciente, renombrar automáticamente, añadir fecha y hora, comparar tamaño, hash o byte a byte. Se puede aplicar la decisión al caso actual o a todos los restantes. Nunca se consideran idénticos dos archivos solo por tener el mismo tamaño.

---

## 14. DUPLICADOS E IMÁGENES SIMILARES

### 14.1 Duplicados exactos
- **Ámbito:** carpetas, unidades, varias unidades o todas las accesibles.
- **Proceso por etapas:**
  1. agrupar por tamaño;
  2. hash parcial (inicio, medio y final);
  3. hash completo;
  4. comparación byte a byte opcional.
- Se distinguen los enlaces físicos (hardlinks) de las copias reales.
- **Información:** nombre, ruta, tamaño, fecha, número de copias y espacio recuperable.
- **Selección por reglas:** conservar el más antiguo, el más reciente, el de una ruta preferida, etc.
- **Nunca se elimina automáticamente.** Siempre hay vista previa y confirmación, y lo eliminado va a la papelera propia.

### 14.2 Imágenes similares
- Herramienta separada de la de duplicados.
- Usa hash perceptual (pHash o dHash), con umbral de similitud ajustable.
- Agrupa originales, versiones comprimidas, redimensionadas y ligeramente editadas.
- Se presentan en comparación lado a lado con resolución, tamaño y fecha.
- Nada se elimina sin selección manual y confirmación.

---

## 15. ARCHIVOS COMPRIMIDOS
- **Formatos:** ZIP, RAR, 7z, TAR, GZ e ISO.
- **Navegación** como carpeta virtual: listar, previsualizar, extraer selección o todo, y copiar hacia fuera.
- **Crear** ZIP, 7z, TAR y TAR.GZ.
- **Añadir y eliminar elementos** solo en los formatos con soporte de escritura seguro, usando un contenedor temporal reconstruido y sustituido de forma atómica al final.
- **RAR:** solo lectura y extracción. **No** se crea ni se modifica RAR.
- **ISO:** solo lectura.
- **Protecciones:** archivos corruptos, rutas de extracción inseguras (`../`, rutas absolutas, enlaces que salen del destino), sobrescrituras accidentales, bombas de descompresión (límites de proporción, tamaño total y número de entradas) y consumo excesivo de recursos.

---

## 16. COMPARACIÓN Y SINCRONIZACIÓN
- **Modos:** comparación, sincronización unidireccional y sincronización bidireccional. Se integran con el doble panel.
- **Estados visibles:** idéntico, diferente, ausente, más reciente y conflicto.
- **Criterios:** nombre, tamaño, fecha, hash o contenido.
- **Sincronización bidireccional:** guarda el estado de la sincronización anterior por cada par de carpetas, para distinguir "eliminado en un lado" de "nuevo en el otro". Sin una política explícita de eliminación elegida por el usuario, nunca propaga borrados.
- **Antes de aplicar:** simulación obligatoria.
- **Durante y después:** registro en el historial; lo eliminado va a la papelera propia.

---

## 17. ETIQUETAS Y COLORES
- Se almacenan en una base de datos local, sin modificar los archivos ni depender de atributos de NTFS, APFS, ext4 o FAT.
- Se conservan al mover o renombrar desde la aplicación.
- **Cambios externos:** se reidentifican los archivos por ID de archivo o inodo + volumen, y tamaño y fecha como respaldo. Si un archivo no puede identificarse sin ambigüedad, la etiqueta queda huérfana y se puede revisar; nunca se reasigna por adivinanza.
- Las etiquetas sirven para buscar y filtrar.

---

## 18. TECLADO Y ACCESIBILIDAD
- Toda función esencial es accesible sin ratón.
- **Atajos predeterminados:**

  | Acción | Windows / Linux | macOS |
  |---|---|---|
  | Copiar / Cortar / Pegar | Ctrl+C / Ctrl+X / Ctrl+V | ⌘C / ⌘X / ⌘V |
  | Seleccionar todo | Ctrl+A | ⌘A |
  | Buscar | Ctrl+F | ⌘F |
  | Imprimir | Ctrl+P | ⌘P |
  | Renombrar | F2 | Enter |
  | Atrás / Adelante | Alt+← / Alt+→ | ⌘[ / ⌘] |
  | Subir un nivel | Alt+↑ | ⌘↑ |
  | Pestañas | Ctrl+T, Ctrl+W, Ctrl+Tab | ⌘T, ⌘W, ⌃Tab |

- Se evitan los atajos reservados por cada sistema.
- Los atajos son personalizables, con detección de conflictos.
- Indicadores de foco visibles y soporte de tecnologías de asistencia mediante Qt Accessibility.

---

## 19. IMPRESIÓN
- Ctrl+P (⌘P en macOS) sobre un archivo compatible (imagen, PDF, texto o código) abre la vista previa con selección de impresora, páginas, orientación y copias, usando el sistema de impresión nativo.
- Para formatos sin impresión integrada, se ofrece la aplicación asociada o se informa de la limitación.
- Nunca se abren documentos potencialmente peligrosos sin advertencia.

---

## 20. CONFIGURACIÓN Y PERSISTENCIA
- **Se conserva:** tema, idioma, favoritos, etiquetas, sesiones, pestañas, paneles, preferencias de miniaturas, reglas de transferencia y de organización, búsquedas guardadas, atajos y límites de concurrencia.
- **Ubicación:** la estándar de cada sistema (`QStandardPaths`).
- **Formato:** archivos de configuración legibles y bases SQLite, con **migraciones versionadas**, de modo que versiones nuevas lean los datos de versiones anteriores.
- Se puede exportar e importar la configuración.

---

## 21. SEGURIDAD Y CONFIABILIDAD
- Manejo correcto de permisos.
- Protección ante operaciones parciales (historial con write-ahead).
- Detección de errores de lectura y escritura.
- Manejo de desconexiones inesperadas.
- Registro rotativo de errores.
- Operaciones atómicas cuando estén disponibles.
- Tratamiento seguro de enlaces simbólicos y prevención de ciclos.
- Compatibilidad con Unicode y con nombres válidos en cada plataforma.
- **macOS:** guía para conceder "Acceso total al disco" cuando el usuario quiera indexar o buscar en todo el disco.
- **Telemetría:** ninguna.

---

## 22. PRUEBAS Y CALIDAD
- **Tipos de prueba:** unitarias, de integración, de operaciones sobre directorios temporales, de concurrencia, de cancelación, de recuperación ante fallos (simulando interrupciones a mitad de operación), con archivos grandes, con más de 100 000 archivos, con rutas largas, con Unicode (NFC/NFD, emoji, RTL), con permisos, con enlaces simbólicos y ciclos, y con archivos comprimidos maliciosos (zip-slip, bombas).
- **Herramientas en CI:** sanitizers (ASan, UBSan, TSan) y análisis estático (clang-tidy).
- **Benchmarks reproducibles:** búsqueda, listado, transferencia, memoria, CPU, miniaturas y latencia de la interfaz. Los resultados van en `docs/BENCHMARKS.md`.
- **CI en GitHub Actions:** Ubuntu 22.04 y 24.04, Windows y macOS (Intel y ARM). Compilación más pruebas en cada push.
- **Pruebas manuales de la interfaz:** en Ubuntu (local, X11 y Wayland) y en la VM de Windows. En macOS solo se verifica la CI; las funciones de interfaz se marcan como **"no verificadas manualmente en macOS"** hasta que exista un equipo Mac.
- No inventes resultados. Lo no medido o no probado se marca como pendiente.

---

## 23. COMPILACIÓN Y DISTRIBUCIÓN
- **Windows:** ejecutable más instalador (Inno Setup o WiX), con icono. El servicio opcional se incluye pero no se instala por defecto.
- **macOS:** `.app` dentro de un `.dmg`, con icono. Sin firma; documentar cómo abrirlo (clic derecho → Abrir, o Ajustes → Privacidad y seguridad).
- **Linux:** paquete `.deb` para Ubuntu y AppImage. Archivos `.desktop` e iconos freedesktop.
- **Artefactos:** se generan en GitHub Actions para cada versión etiquetada.
- **Documentación:** compilación reproducible en `README.md`; licencias de terceros en `THIRD_PARTY_LICENSES.md`.
- No se afirma que una compilación funciona en un sistema donde no se ha probado.

---

## 24. FASES DE DESARROLLO

Cada fase entrega un resultado funcional y verificable, con sus criterios de aceptación cumplidos (§26) y `docs/STATUS.md` actualizado.

### FASE 0 — Arquitectura y entorno
- Instalar el entorno en Ubuntu.
- Crear la VM de Windows.
- Crear el repositorio y la CI de las tres plataformas compilando un "hola mundo" Qt.
- Elegir Widgets o Qt Quick, con un ADR que lo justifique.
- Definir la arquitectura, la estructura de carpetas y las dependencias con sus licencias.
- Diseñar el icono y el mockup de la interfaz.
- Preparar las herramientas de benchmark, medir la línea base y ajustar las metas.
- Elaborar el plan de pruebas.
- Evaluar riesgos y restricciones por sistema operativo.

**⏸ Detente al terminar la Fase 0** y presenta un informe para que el usuario lo apruebe antes de continuar.

### FASE 1 — Explorador funcional
- Ventana, árbol de directorios y detección de unidades.
- Navegación e historial.
- Vista detallada virtualizada con ordenamiento natural.
- Tamaño de carpetas.
- Operaciones básicas, papelera propia y historial con deshacer.
- Atajos principales.
- Manejo de errores.
- Pestañas básicas.

### FASE 2 — Búsqueda y rendimiento
- Planificador concurrente.
- Búsqueda progresiva con comodines, operadores, filtros y expresiones regulares.
- Filtrado local.
- Índice SQLite con vigilancia de cambios y asistente de primer arranque.

### FASE 3 — Miniaturas, previsualización y metadatos
- Imágenes (incluye SVG y RAW), documentos (con LibreOffice opcional), video y audio.
- Caché de miniaturas.
- Panel de metadatos.

### FASE 4 — Herramientas
- Renombrado masivo.
- Organizador automático.
- Transferencias con verificación.
- Duplicados.
- Imágenes similares.
- Etiquetas.
- Archivos comprimidos.
- Búsqueda en contenido (FTS5).

### FASE 5 — Funciones profesionales
- Columnas Miller.
- Doble panel.
- Comparación y sincronización.
- Pestañas avanzadas y restauración de sesiones.
- Personalización completa.
- Impresión.
- Servicio opcional de Windows (MFT/USN).

### FASE 6 — Optimización y distribución
- Pruebas integrales y corrección de errores.
- Optimización de memoria y concurrencia contra las metas de rendimiento.
- Empaquetado de las tres plataformas.
- Documentación de usuario.

**Futuro (documentado, sin implementar):** monitor de rendimiento integrado (CPU, RAM, disco, archivos/s, MB/s y trabajadores).

El orden puede ajustarse por dependencias técnicas; registra cualquier cambio en `docs/DECISIONS.md`. A partir de la Fase 1 continúa de una fase a la siguiente sin pedir aprobación, informando al cierre de cada fase, salvo que necesites una decisión que solo el usuario puede tomar.

---

## 25. REGLAS OBLIGATORIAS
1. No crees funciones simuladas ni botones decorativos presentados como funcionales. Si algo no está implementado, se deshabilita y se indica "próximamente".
2. No uses pseudocódigo en lugar de código.
3. Mantén la modularidad descrita en §3.
4. Documenta las decisiones técnicas en `docs/DECISIONS.md`.
5. Usa nombres descriptivos y comentarios en español que expliquen el porqué.
6. Gestiona todos los errores; ninguna excepción debe quedar sin capturar en los límites entre hilos.
7. No elimines funcionalidad existente al añadir otra nueva.
8. Mantén compatibilidad con los datos de versiones anteriores mediante migraciones.
9. No hagas cambios innecesarios en módulos estables.
10. No inventes resultados de pruebas ni de rendimiento.
11. Mantén al día `docs/STATUS.md`, distinguiendo Terminada, Parcial, Pendiente y Bloqueada.
12. Si algo es imposible bajo alguna restricción, explica la causa y propone una alternativa.
13. Prefiere bibliotecas mantenidas y compatibles con GPL-3.
14. Nunca comprometas la integridad de los archivos por optimizaciones inseguras.
15. Haz commits pequeños y descriptivos, en ramas por fase o función, y fusiónalos solo con la CI en verde.

---

## 26. CRITERIOS DE ACEPTACIÓN
Una función se considera **Terminada** solo si cumple todo lo siguiente:

- tiene implementación real, integrada con sus módulos;
- funciona con entradas válidas y maneja los errores esperables;
- no bloquea la interfaz más allá de la meta de §4;
- tiene pruebas automatizadas que pasan en la CI;
- está documentada;
- no introduce regresiones conocidas;
- se verificó manualmente al menos en Ubuntu, y además en Windows si depende de la plataforma.

Si falta cualquiera de estas condiciones, la función es **Parcial** y debe indicarse qué falta. La apariencia visual nunca basta para dar una función por terminada.

---

## 27. INSTRUCCIÓN INICIAL
Comienza por la **FASE 0**. Al terminarla, entrega un informe que incluya:

- **A.** Análisis de viabilidad y riesgos por sistema operativo.
- **B.** Ambigüedades o contradicciones que aún detectes en esta especificación, con propuestas de solución.
- **C.** Confirmación técnica de C++20/Qt 6 y la elección entre Widgets y Qt Quick, justificadas.
- **D.** Diseño conceptual de la interfaz (mockup) y el icono.
- **E.** Estructura de carpetas y módulos.
- **F.** Plan detallado de las fases 1 a 6, con criterios de aceptación por fase.
- **G.** Solo las preguntas cuya respuesta cambie la arquitectura o las restricciones.
- **H.** Estado del entorno: lo instalado, la VM de Windows, el repositorio y la CI.

Después, espera la aprobación del usuario para iniciar la Fase 1.

---

## 28. VISIÓN
Explorador Ultra combina:

- la facilidad del Explorador de Windows;
- la navegación de Finder;
- la productividad de los administradores profesionales de doble panel;
- un motor de búsqueda de alto rendimiento;
- herramientas integradas de gestión de archivos;
- seguridad y confiabilidad.

No se trata de acumular funciones, sino de construir una aplicación rápida, intuitiva, robusta y coherente, en la que cada función esté realmente terminada.
