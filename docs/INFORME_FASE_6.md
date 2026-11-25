# Informe de la Fase 6 — Optimización y distribución

Fecha: 2026-11-25 · Estado: **entregada con puntos abiertos**. Las metas de rendimiento se midieron con la ventana real en Ubuntu y Windows, se corrigieron los fallos que salieron al medir, y la CI fabrica y prueba paquetes para los tres sistemas. Es la primera versión etiquetada: **v0.1.0**.

## A. Qué hay
| Área | Qué hace |
|---|---|
| Medición con la ventana real | `eu-bench-ui open|cancel <carpeta>`: abre una carpeta en una `MainWindow` de verdad y mide los primeros elementos, el listado completo, el bloqueo de la interfaz, la memoria, ordenar, releer con todo seleccionado y cancelar (`BENCHMARKS.md`) |
| Paquetes de Linux | `.deb` para Ubuntu 22.04 y 24.04 (en `/opt/explorador-ultra`, con su Qt al lado y dependencias calculadas) y AppImage |
| Instalador de Windows | Inno Setup en español; para todos los usuarios o solo el actual; el servicio opcional se copia pero no se instala, y se quita al desinstalar |
| macOS | `.dmg` para Intel y Apple Silicon |
| CI de paquetes | `.github/workflows/paquetes.yml`: en cada etiqueta `v*` fabrica todos, prueba cada uno arrancando sin el Qt de la compilación y pidiendo una miniatura, y deja un borrador de versión en GitHub |
| Documentación | `README.md`, `THIRD_PARTY_LICENSES.md`, `GUIA_DE_USUARIO.md` y `DISTRIBUCION.md` (SmartScreen, Gatekeeper, «Acceso total al disco») |

## B. Criterios de aceptación de la fase (`PHASE_PLAN.md`)
| Criterio | Estado |
|---|---|
| Metas de §4 medidas y anotadas; las incumplidas, con causa | **Cumplido** en Ubuntu y Windows (VM). Se cumplen todas las metas medidas salvo el objetivo ideal de 16 ms de bloqueo al abrir (19–48 ms) y dos casos que pasan de 50 ms: aplicar un orden nuevo (44–85 ms) y releer con 100 000 seleccionados (56–84 ms). La causa está en `BENCHMARKS.md`: el modelo se reinicia entero con 100 000 filas |
| Pruebas integrales y sanitizers en verde; sin regresiones conocidas | **Cumplido**: CI verde en las seis plataformas, ASan+UBSan incluido |
| Artefactos de la CI en cada etiqueta, probados | **Cumplido**: el `.deb` lo instaló el dueño en Ubuntu 26.04, el AppImage se probó en Ubuntu, el instalador en la VM de Windows y en la CI (instala y desinstala). El `.dmg` solo se compila y se arranca en la CI, como prevé el plan |
| README, licencias, guía de usuario y guías de seguridad | **Cumplido** |

## C. Verificación realizada
- **CI**: Ubuntu 22.04 y 24.04, Ubuntu con ASan+UBSan, Windows (MSVC), macOS Intel y Apple Silicon, todo en verde. Flujo de paquetes en verde en sus cinco trabajos.
- **Ubuntu**: 35/35 pruebas. **VM de Windows 11**: 36/36.
- **A mano**: AppImage en Ubuntu 26.04; `.deb` instalado por el dueño; instalador en la VM (asistente en español, arranque, miniaturas, icono).

## D. Fallos encontrados en la fase
| Qué | Cómo apareció | Estado |
|---|---|---|
| Con muchos elementos seleccionados, releer la carpeta congelaba la ventana durante minutos (coste cuadrático) | Al medir | Corregido: menos de 1 s con 100 000 seleccionados |
| En el paquete, el auxiliar de miniaturas no arrancaba (faltaba la plataforma offscreen de Qt): un aviso por foto | Al instalar en la VM | Corregido en todos los sistemas; la CI lo comprueba en cada paquete |
| Un cierre del auxiliar marcaba la foto como ilegible para siempre | Mismo caso | Corregido: solo se recuerda durante la sesión (`ErrorCode::Crashed`) |
| El `.exe` y la app de macOS no llevaban icono | Al instalar en la VM | Corregido |
| `test_core_crash_report` fallaba ~1 de cada 10 veces: el informe leía fuera de la pila | Al medir | Corregido (0 de 60) |
| La copia al portapapeles de la sesión remota no llegaba al escritorio | El dueño (entorno de trabajo, no la app) | Corregido fuera del proyecto |

## D2. Versión 0.1.1 (25-nov)
| Aviso del dueño | Qué era | Qué se hizo |
|---|---|---|
| Al sincronizar dos discos: «El archivo o la carpeta no existe … .eu-parcial» | Se empezó una segunda sincronización de las mismas carpetas mientras la primera seguía copiando (desde otra ventana). La comparación tomó el temporal de una copia en curso por un archivo real | La comparación ignora los temporales `.eu-parcial` y `.eu-punto`, y no se compara ni sincroniza un par mientras otra sincronización trabaja en él (`FileActions::isSynchronizing`) |
| (Encontrado al investigarlo) | Las copias de archivos de **solo lectura** quedaban con la fecha del día: los permisos se ponían antes que la fecha. Al sincronizar en los dos sentidos parecían más nuevas y se copiaban de vuelta, reemplazando el original (a la papelera, con el mismo contenido) | La fecha se pone antes que los permisos. Las fechas de los 4 archivos afectados se restauraron a mano |

## E. Lo que queda abierto
1. **Medir con caché fría, en disco mecánico y en USB** con la ventana real, y en macOS.
2. **Bajar los bloqueos de 44–85 ms** al aplicar un orden o releer con todo seleccionado: pediría insertar las filas por partes en vez de reiniciar el modelo.
3. **Probar el `.dmg` en un Mac real**, y la firma de código (sin firmar, Windows y macOS avisan; ver `DISTRIBUCION.md`).
4. Siguen abiertos los puntos de los informes anteriores.

## F. Siguiente
Con la Fase 6 termina el plan (`PHASE_PLAN.md`). Lo siguiente lo decide el dueño a partir de su uso de la v0.1.0.
