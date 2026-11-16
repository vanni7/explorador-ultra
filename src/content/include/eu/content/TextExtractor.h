// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/tasks/Cancel.h"

#include <QString>

namespace eu::content {

/// Formatos de los que se sabe sacar el texto (SPEC §6.5).
enum class TextSource {
    None,
    Plain,      ///< TXT, código, Markdown, CSV… (UTF-8, UTF-16 o Latin-1).
    Pdf,        ///< Con Qt PDF.
    Word,       ///< DOCX.
    OpenText,   ///< ODT, ODS, ODP (OpenDocument).
    Excel,      ///< XLSX.
    PowerPoint, ///< PPTX.
    Rtf,
};

/// De qué clase es, por la extensión. Solo los que esta compilación sabe leer.
TextSource textSourceFor(const QString &fileName);

struct ExtractLimits
{
    /// Archivos mayores que esto no se leen.
    qint64 maxFileBytes = 100LL * 1000 * 1000;
    /// Del texto se guarda como mucho esto (un registro de varios GB no llena la memoria).
    qint64 maxTextChars = 20LL * 1000 * 1000;
};

/// El texto del archivo, para buscar en él (SPEC §6.5). Nunca ejecuta nada
/// del documento (macros, scripts): solo lee su texto. Bloqueante; se llama
/// desde una tarea (y, para lo que no es texto plano, en el proceso auxiliar).
core::Result<QString> extractText(const QString &path, const tasks::CancelToken &cancel,
                                  const ExtractLimits &limits = {});

/// Un fragmento del texto alrededor de la primera vez que aparece `needle`,
/// para enseñarlo en los resultados. `start`/`length` dicen dónde está la
/// coincidencia dentro del fragmento (para resaltarla). Vacío si no aparece.
/// Lleva poco texto antes y más después, para que la coincidencia se vea
/// aunque la columna sea estrecha.
struct Snippet
{
    QString text;
    int start = 0;
    int length = 0;
};
Snippet snippetAround(const QString &text, const QString &needle, Qt::CaseSensitivity sensitivity,
                      int before = 30, int after = 90);

} // namespace eu::content
