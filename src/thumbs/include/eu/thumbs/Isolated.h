// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/meta/Metadata.h"
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

/// Como meta::readMetadata, pero leyendo en el proceso auxiliar: el EXIF de
/// una foto o las etiquetas de un audio hechos para romper el lector no
/// cierran la aplicación. Si el auxiliar se cierra o se cuelga, la lista
/// viene vacía. Sin auxiliar, se lee en este proceso.
QList<meta::Section> readMetadataIsolated(const QString &path, const tasks::CancelToken &cancel);

/// El texto de un documento para buscar en él (content::extractText), leído
/// en el proceso auxiliar si no es texto plano: un PDF o un DOCX dañados no
/// cierran la aplicación.
core::Result<QString> extractTextIsolated(const QString &path, const tasks::CancelToken &cancel);

/// true si está el auxiliar (si no, renderThumbnailIsolated no aísla).
bool hasThumbnailHelper();

/// Tiempo máximo por archivo antes de dar por colgado al auxiliar (30 s).
void setThumbnailHelperTimeout(std::chrono::milliseconds timeout);

} // namespace eu::thumbs
