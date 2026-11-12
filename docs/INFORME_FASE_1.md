# Informe de la Fase 1 — Explorador funcional

Fecha: 2026-11-12 · Estado: **entregada con puntos abiertos**. El explorador navega y hace todas las operaciones básicas con historial y deshacer, con pruebas en verde en las tres plataformas. Según el criterio de §26, ninguna función está todavía "Terminada": a todas les falta parte de la verificación manual (sobre todo en Windows y en X11) y hay tres criterios de la fase sin medir. El detalle por función está en [`STATUS.md`](STATUS.md).

## A. Qué hay
| Área | Qué hace |
|---|---|
| Navegación | Pestañas con sesión, árbol de lugares (favoritos y unidades), barra de ruta con migas, historial atrás/adelante, vista detallada virtualizada con orden natural |
| Operaciones | Crear carpeta y archivo, renombrar en línea, copiar, cortar, pegar, mover (también entre unidades), enviar a la papelera, eliminar permanentemente con confirmación reforzada |
| Arrastrar y soltar | Sobre carpetas de la lista y del árbol; Ctrl copia, Mayús mueve; selección con recuadro |
| Seguridad | Nunca se sobrescribe, nunca se siguen enlaces al borrar, cada paso se anota en el historial antes de tocar el disco |
| Deshacer | Ctrl+Z deshace la última operación; el historial sobrevive a reinicios |
| Recuperación | Al arrancar se detectan las operaciones interrumpidas y se ofrece revertir o descartar |
| Papelera | Propia, en el mismo volumen; ventana única para restaurar, eliminar o vaciar |
| Tamaño de carpetas | Opcional, en segundo plano, progresivo y cancelable |

## B. Criterios de aceptación de la fase
| Criterio | Estado |
|---|---|
| 100 000 archivos: primeros elementos < 100 ms y listado ordenado < 1 s | **Cumplido** en este equipo (21 ms y ~365 ms, SSD) |
| Bloqueo de interfaz < 50 ms | **Sin medir** |
| Orden natural; tamaños SI con opción binaria; tamaño de carpetas progresivo y cancelable | **Cumplido** (pruebas) |
| Copiar, mover (entre volúmenes), renombrar y papelera, reversibles con Deshacer y persistentes | **Cumplido** en pruebas automáticas; el dueño lo usó a mano en Ubuntu |
| Matar la aplicación a mitad de un mover no pierde ni duplica datos; se ofrece reanudar, revertir o descartar | **Parcial**: revertir y descartar funcionan (prueba que mata el proceso); **reanudar no existe** |
| Papelera en el mismo volumen y restaura sin base de datos | **Cumplido** (pruebas) |
| Conectar y desconectar una USB actualiza el árbol sin congelar | **Sin verificar a mano** |
| Enlaces, ciclos, permisos, rutas largas y Unicode en las tres plataformas | **Parcial**: en Windows se omiten las pruebas de enlaces y permisos; rutas largas de Windows sin prueba |
| Verificado a mano en Ubuntu (Wayland y X11) y en la VM de Windows | **Parcial**: el dueño probó en Wayland; X11 solo con ratón simulado en una pantalla virtual; **en Windows nadie ha usado la ventana** (solo pruebas automáticas y arranque) |

## C. Verificación realizada
- **CI** (GitHub Actions): Ubuntu 22.04 y 24.04, Ubuntu con ASan+UBSan, Windows (MSVC), macOS Intel y Apple Silicon.
- **Local**: 16 programas de prueba en release, ASan+UBSan y TSan (este último sin las pruebas con ventana).
- **VM de Windows 11** (zona horaria de México): las 16 pruebas y el arranque.
- **Prueba manual del dueño** en Ubuntu 26.04 (Wayland), 12-nov: funcionó lo básico y salieron cinco observaciones, todas corregidas: no se podían abarcar varios archivos con el ratón, no se podía soltar sobre el árbol, el árbol no se desplegaba con un clic, la lista no tenía iconos y se pidió que el clic en la ruta la copiara.

## D. Fallos encontrados durante la fase
| Fallo | Cómo apareció | Estado |
|---|---|---|
| En Windows, con zonas que tuvieron horario de verano (México), copiar desplazaba la fecha de modificación de archivos de aquellos veranos | Al pasar las pruebas en la VM; la CI corre en UTC y no lo veía | Corregido: las fechas van en UTC |
| Lo borrado en una unidad que no se muestra en el panel (p. ej. `/tmp` en memoria) iba a una papelera que la ventana no enseñaba | Prueba de la ventana de la papelera | Corregido |
| Deshacer se podía quedar atascado en una operación que no se dejaba deshacer del todo | Revisión al escribir `FileActions` | Corregido: esas se saltan |
| Una segunda instancia habría tomado por interrumpidas las operaciones en curso de la primera | Revisión del diseño | Corregido con un cerrojo sobre el historial |

## E. Lo que queda abierto (pasa a la lista de pendientes, no bloquea la Fase 2)
1. Usar la ventana a mano en Windows y en una sesión X11 real.
2. Medir el bloqueo del hilo de interfaz y el coste del tamaño de carpetas en un disco mecánico.
3. Probar a mano conectar y desconectar una USB, y la papelera en una USB.
4. "Reanudar" una operación interrumpida.
5. Marcar visualmente lo cortado; "Abrir con…" y "Propiedades".
6. Papelera: retención, tamaño máximo y unidades desconectadas como "no disponibles".
7. Ordenar por el tamaño calculado de las carpetas.
8. Políticas de colisión completas (sobrescribir, preguntar): Fase 4.

## F. Siguiente fase
Fase 2 — Búsqueda y rendimiento. Plan y criterios en [`PHASE_PLAN.md`](PHASE_PLAN.md).
