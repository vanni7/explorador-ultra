# Riesgos y restricciones por sistema operativo

Gravedad: Alta (puede obligar a cambiar el diseño o el alcance), Media, Baja.

## Generales
| Riesgo | Gravedad | Mitigación |
|---|---|---|
| Alcance muy grande (23 módulos) para un mantenedor que no programa en C++ | Alta | Fases con entregables utilizables; código explícito y documentado; nada se marca "Terminada" sin pruebas |
| Pérdida de datos por errores en mover, papelera o sincronización | Alta | Historial previo a cada operación, pruebas de interrupción, simulación obligatoria, nunca borrar el origen antes de sincronizar a disco |
| Decodificadores (FFmpeg, LibRaw, PDF) ante archivos maliciosos o dañados | Media | Proceso auxiliar aislado con límite de tiempo y memoria para miniaturas y previsualización |
| Qt 6.12 podría no admitir Ubuntu 22.04 o macOS 13 | Media | Se sabrá en la primera ejecución de la CI; alternativa: Qt 6.8.3 en esas plataformas |
| `aqtinstall` 3.3.0 no instala Qt 6.12 en Windows (cambió la estructura del repositorio de Qt) | Media | Guion propio `tools/windows/fetch_qt_windows.py`; en la CI puede hacer falta lo mismo |
| Sin firma de código: avisos de SmartScreen y Gatekeeper | Media | Documentar cómo autorizar; canal preparado para firmar en el futuro |

## Linux
| Riesgo | Gravedad | Mitigación |
|---|---|---|
| Wayland: arrastrar y soltar entre aplicaciones, posición de ventanas y atajos globales tienen limitaciones | Media | Probar en Wayland y X11; no depender de posicionamiento absoluto |
| Límite de inotify (`max_user_watches`) insuficiente para indexar discos grandes | Media | Detectarlo, explicar cómo ampliarlo y, donde haya permisos, ofrecer fanotify; nuevo recorrido periódico como respaldo |
| La papelera en `/` no puede ir en la raíz | Baja | Ya previsto en §10.2: `~/PapeleraExploradorUltra` verificando el dispositivo |
| Discos NTFS/exFAT montados (este equipo tiene varios): sin permisos POSIX, nombres sin distinguir mayúsculas | Media | Detectar el sistema de archivos por volumen y adaptar validación de nombres y colisiones |
| El equipo de desarrollo es 26.04 y los destinos son 22.04/24.04 | Baja | CI en ambos; paquetes generados en 22.04 |

## Windows
| Riesgo | Gravedad | Mitigación |
|---|---|---|
| Servicio MFT/USN: requiere privilegios, es código delicado y los antivirus pueden marcarlo (más aún sin firma) | Alta | Proceso separado y mínimo, solo lectura, opcional, al final (Fase 5); la aplicación funciona sin él |
| Rutas largas y nombres reservados (`CON`, `NUL`, punto o espacio final) | Media | Prefijo `\\?\`, manifiesto *longPathAware*, validación en `core` |
| Archivos bloqueados por otros procesos | Media | Informar del proceso cuando sea posible (Restart Manager), reintentar u omitir |
| La VM es una evaluación de 90 días | Baja | Procedimiento de reinstalación documentado; la CI no depende de ella |
| Windows 10 22H2 ya no recibe soporte de Microsoft | Baja | Se compila para Windows 10 (Qt 6.12 lo admite, por verificar), pero solo se prueba a mano en Windows 11 |

## macOS
| Riesgo | Gravedad | Mitigación |
|---|---|---|
| No hay equipo Mac: la interfaz no se puede probar a mano | Alta | Solo CI; todo lo visual queda "no verificado manualmente en macOS" hasta tener un Mac |
| Permisos (TCC): sin "Acceso total al disco" no se ven Escritorio, Documentos, etc. | Media | Guía dentro de la aplicación (§21) |
| Sin firma ni notarización, Gatekeeper bloquea por defecto (y cada versión de macOS lo endurece) | Media | Documentar la autorización; advertir que podría dejar de bastar |
| Nombres en NFD (HFS+) frente a NFC (APFS conserva lo que recibe) | Media | Normalizar al comparar y buscar, nunca al escribir |
| Binario universal: todas las dependencias de vcpkg deben compilarse para las dos arquitecturas | Media | Compilar por separado y unir con `lipo`; si no es viable, dos descargas |
