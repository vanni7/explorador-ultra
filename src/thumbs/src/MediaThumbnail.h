// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/thumbs/Thumbnail.h"

namespace eu::thumbs::detail {

/// true si esta compilación sabe leer video y audio (FFmpeg 6.1 o posterior).
bool hasMediaSupport();

/// Video: el fotograma del segundo 3 (o de la mitad, si dura menos), girado
/// según los metadatos del móvil, con la duración en una esquina.
/// Audio: la carátula incrustada (o una genérica) con la duración.
core::Result<QImage> renderMedia(const ThumbnailRequest &request, const tasks::CancelToken &cancel, bool audio);

} // namespace eu::thumbs::detail
