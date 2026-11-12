# Informe de la Fase 0 — Arquitectura y entorno

Fecha: 2026-11-11 · Estado: **entregada con un punto abierto**: el repositorio en GitHub y la CI no existen todavía porque falta la autenticación de `gh`. macOS, por tanto, no se ha compilado nunca.

## A. Viabilidad y riesgos por sistema operativo
El proyecto es viable con C++20 + Qt 6.12. Lo más delicado no es técnico sino de alcance: 23 módulos y un único mantenedor. El detalle está en [`RISKS.md`](RISKS.md); los riesgos altos son:

| Sistema | Riesgo principal |
|---|---|
| Todos | Integridad de datos en mover, papelera y sincronización → historial previo, simulación y pruebas de interrupción |
| Linux | Wayland limita arrastrar y soltar entre aplicaciones; límites de inotify al indexar discos grandes |
| Windows | El servicio MFT/USN es código con privilegios y sin firma: los antivirus pueden marcarlo. Va al final y es opcional |
| macOS | No hay Mac: nada de la interfaz se puede verificar a mano. Sin firma, Gatekeeper bloquea por defecto |

## B. Ambigüedades y contradicciones de la especificación
| # | Punto | Propuesta |
|---|---|---|
| 1 | §1 fija Ubuntu 22.04 y 24.04, pero este equipo ya es **26.04** | Añadir 26.04 como plataforma oficial. 22.04 y 24.04 se cubren en la CI (ADR-0008) |
| 2 | "La LTS más reciente" es Qt 6.12, que podría no admitir Ubuntu 22.04, macOS 13 o Windows 10 | Se sabrá al ejecutar la CI. Si falla, elegir entre Qt 6.8.3 (sin parches públicos) o subir los mínimos |
| 3 | Windows 10 22H2 es plataforma mínima, pero Microsoft dejó de darle soporte en octubre de 2025 y la VM es Windows 11 | Compilar para Windows 10 sin probarlo a mano, o crear una segunda VM de Windows 10 |
| 4 | §4: "medir contra el explorador nativo" no se puede automatizar con fiabilidad (Archivos de GNOME, Explorador de Windows, Finder) | Usar como referencia las herramientas del sistema (`gio list`, `cp`, `robocopy`) y decirlo así en `BENCHMARKS.md` |
| 5 | El equipo de referencia (4 núcleos, 8 GB) no existe; este es mucho más potente | Aproximarlo limitando núcleos y memoria con cgroups, y con la VM de Windows (4 CPU, 8 GB) |
| 6 | §10: una papelera visible llamada `PapeleraExploradorUltra` en la raíz de cada disco; en NTFS/exFAT "oculta" depende del sistema que la mire | Nombre con punto inicial en Linux/macOS más atributo oculto en Windows; en discos compartidos entre sistemas se aplican ambos |
| 7 | §10.1 "nunca se copia entre discos" frente a §10.2 (volumen sin sitio escribible) | Ya resuelto en §10.2: solo eliminación permanente. Se añade aviso claro en la interfaz |
| 8 | §8.2: renombrar con "clic lento" y abrir con doble clic entran en conflicto por tiempos | Clic lento desactivado por defecto; F2 y menú siempre activos |
| 9 | §18: en macOS `Enter` renombra; en Windows/Linux abre | Se respeta por plataforma; quedará configurable |
| 10 | §15: libarchive lee RAR sin usar unrar; RAR5 cifrado o en varios volúmenes puede fallar | Informar "formato no admitido" en esos casos; no se añade unrar (licencia no libre) |
| 11 | §25.15 "fusionar solo con la CI en verde" no se puede cumplir hasta tener GitHub | El primer commit está solo en local en `main`; desde que exista el remoto, ramas y CI |
| 13 | `aqtinstall` 3.3.0 no encuentra Qt 6.12 para Windows; la acción de la CI (`install-qt-action`) usa esa herramienta | Es probable que el trabajo de Windows de la CI falle al instalar Qt. Alternativas: una versión más nueva de aqtinstall si existe, el guion propio `fetch_qt_windows.py`, o Qt 6.11 solo en Windows |
| 12 | §6.1 `type:imagen` y `hidden:yes` mezclan español e inglés | Aceptar ambos idiomas en valores (`imagen`/`image`, `sí`/`yes`) |

## C. Confirmación técnica
- **C++20 + Qt 6.12.0 LTS**: confirmado. Compila con GCC 15.2 y arranca en Ubuntu 26.04 (Wayland y XWayland), con pruebas y ASan+UBSan en verde. También compila y pasa las pruebas en Windows 11 (VM) con MSVC 2022. macOS: sin verificar hasta ejecutar la CI.
- **Qt Widgets** en lugar de Qt Quick (ADR-0002): vistas virtualizadas maduras, arrastrar y soltar, accesibilidad e impresión ya resueltos, un solo lenguaje y sin exigir GPU.
- **Listado con API nativa** (ADR-0005), respaldado por la medición: `QDirIterator` es 3,8 veces más lento que `readdir`+`fstatat` con 100 000 archivos.

## D. Interfaz e icono
- Mockup: [`mockup/ventana-principal.png`](mockup/ventana-principal.png) (pestañas, barra con migas de pan y buscador, árbol, vista detallada, metadatos y previsualización, barra de estado con transferencias).
- Icono: [`../assets/icon/explorador-ultra.svg`](../assets/icon/explorador-ultra.svg) — carpeta con brújula cuya aguja es un rayo. Generados PNG (16 a 1024 px), `.ico` y `.icns` con `tools/generate_icons.sh`.

## E. Carpetas y módulos
Ver [`ARCHITECTURE.md`](ARCHITECTURE.md): estructura de carpetas, 23 módulos en 6 capas, `Location`, modelo de hilos y persistencia. Hoy existen `core`, `app`, `tests/core` y `bench`.

## F. Plan de las fases 1 a 6
Ver [`PHASE_PLAN.md`](PHASE_PLAN.md), con criterios de aceptación medibles por fase.

## G. Preguntas que cambian la arquitectura o las restricciones
1. **GitHub:** ¿con qué cuenta y nombre de repositorio se publica? Debe ser público para tener macOS gratis en la CI. Hace falta ejecutar `gh auth login`.
2. **Qt 6.12 y plataformas antiguas:** si la CI muestra que Qt 6.12 no admite Ubuntu 22.04, macOS 13 o Windows 10, ¿se prefiere bajar a Qt 6.8.3 o subir los mínimos?
3. **Ubuntu 26.04:** ¿se añade como plataforma oficial?
4. **Windows 10:** ¿hace falta probarlo a mano (segunda VM) o basta compilar para él?
5. **Autoría de los commits:** usan la cuenta de GitHub del proyecto con su correo `noreply`; ¿qué nombre y correo deben figurar en el repositorio público?

## H. Estado del entorno
| Elemento | Estado |
|---|---|
| Herramientas en Ubuntu | Instaladas: CMake 4.2.3, Ninja, GCC 15.2, Clang 21.1.6, clang-tidy, ccache, Valgrind, `gh`, hyperfine. Detalle en [`ENVIRONMENT.md`](ENVIRONMENT.md) |
| Qt | 6.12.0 en `~/Qt` (aqtinstall), con Multimedia, PDF e Image Formats |
| Bibliotecas | libarchive, Exiv2, TagLib, LibRaw, FFmpeg, SQLite y xxHash instaladas (aún sin enlazar) |
| Compilación local | En verde: 1 ejecutable de pruebas (6 casos), aplicación mínima y `eu-bench` |
| Línea base | Medida en SSD: listado, orden, filtro y copia ([`BENCHMARKS.md`](BENCHMARKS.md)) |
| VM de Windows | `EU-Win11-Build`: Windows 11 Enterprise de evaluación (90 días) con MSVC 2022, CMake, Git, Python y Qt 6.12. El proyecto compila, pasa las pruebas y arranca (plataforma `offscreen`). No se comprobó la ventana a la vista en el escritorio de Windows. UAC desactivado en esa VM |
| Repositorio | Git local con el primer commit. **Sin remoto en GitHub** |
| CI | Flujo escrito para Ubuntu 22.04/24.04, Windows y macOS (Intel y ARM). **Nunca ejecutado** |
