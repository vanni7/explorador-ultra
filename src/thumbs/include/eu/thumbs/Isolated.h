// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/thumbs/Thumbnail.h"

#include <chrono>

namespace eu::thumbs {

/// Como renderThumbnail, pero decodificando en el proceso auxiliar
/// eu-thumbnailer (SPEC §7.1: lo de riesgo, aislado). Si el archivo hace que
/// el auxiliar se cierre o se cuelgue, aquí solo se recibe un error y el
/// auxiliar se vuelve a arrancar para el siguiente.
///
/// Cada hilo tiene su propio auxiliar, que se reutiliza entre peticiones. Si
/// el auxiliar no está junto al ejecutable, se decodifica en este proceso.
/// Bloqueante: se llama desde una tarea.
core::Result<QImage> renderThumbnailIsolated(const ThumbnailRequest &request, const tasks::CancelToken &cancel);

/// true si está el auxiliar (si no, renderThumbnailIsolated no aísla).
bool hasThumbnailHelper();

/// Tiempo máximo por archivo antes de dar por colgado al auxiliar (30 s).
void setThumbnailHelperTimeout(std::chrono::milliseconds timeout);

} // namespace eu::thumbs
