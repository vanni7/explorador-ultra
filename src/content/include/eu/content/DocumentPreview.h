// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/Error.h"
#include "eu/tasks/Cancel.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace eu::content {

/// Una hoja de cálculo tal como se enseña en la vista previa.
struct SheetPreview
{
    QString name;
    int columns = 0;              ///< Columnas mostradas (A = 1), hasta la última con datos.
    QList<int> rowNumbers;        ///< Número de cada fila mostrada (las vacías no vienen).
    QList<QStringList> cells;     ///< Por fila, el texto de cada columna (puede venir más corta).
    QList<QByteArray> numeric;    ///< Por fila, '1' en las celdas que son números (van a la derecha).
    bool moreRows = false;        ///< Había más filas con datos que las mostradas.
    bool moreColumns = false;     ///< Había datos más allá de la última columna mostrada.
    int totalRows = 0;            ///< Filas de la hoja según el archivo; 0 si no lo dice.
};

/// Un documento de oficina preparado para verlo en el panel de vista previa
/// (SPEC §7.1) sin LibreOffice: se lee su XML directamente.
struct DocumentPreview
{
    /// Word y OpenDocument de texto: HTML propio con solo párrafos, títulos,
    /// negrita, cursiva, subrayado, listas, tablas e imágenes (las de
    /// `images`). Todo el texto del documento va escapado y no hay enlaces:
    /// verlo no carga ni ejecuta nada.
    QString html;
    /// Las imágenes del documento, ya decodificadas y reducidas en el proceso
    /// auxiliar (PNG o JPEG propios). El HTML las nombra «eu-imagen:N».
    QList<QByteArray> images;
    /// Excel y OpenDocument de cálculo: las hojas visibles.
    QList<SheetPreview> sheets;
    int moreSheets = 0;           ///< Hojas visibles que no se muestran por el tope.
    bool truncated = false;       ///< Se cortó algo por los topes (texto, filas, columnas u hojas).

    bool isSpreadsheet() const { return html.isEmpty() && !sheets.isEmpty(); }
};

struct PreviewLimits
{
    qint64 maxFileBytes = 200LL * 1000 * 1000;
    /// XML que se lee como mucho de cada parte (de lo demás, el principio) y en total.
    qint64 maxPartBytes = 48LL * 1000 * 1000;
    qint64 maxSheetBytes = 16LL * 1000 * 1000;
    qint64 maxTotalBytes = 160LL * 1000 * 1000;
    /// Texto que se muestra de un documento de Word u OpenDocument.
    int maxChars = 400000;
    /// Celdas de tabla de un documento de texto (el visor se vuelve lento con muchas).
    int maxTableCells = 6000;
    /// Imágenes de un documento de texto: cuántas, cuánto puede ocupar cada
    /// original y cuánto todas ya reducidas; ancho máximo con que se ven.
    int maxImages = 60;
    qint64 maxImageFileBytes = 24LL * 1000 * 1000;
    qint64 maxImageBytes = 24LL * 1000 * 1000;
    int maxImageWidth = 560;
    /// Hojas de cálculo.
    int maxSheets = 10;
    int maxRows = 500;
    int maxColumns = 40;
};

/// true si se sabe mostrar este tipo así (DOCX, XLSX, ODT, ODS…) en esta compilación.
bool hasDocumentPreview(const QString &fileName);

/// Lee el documento y lo prepara para verlo. Nunca ejecuta nada del
/// documento (macros, campos, enlaces). Bloqueante; se llama desde una tarea
/// y, en la aplicación, en el proceso auxiliar (un documento hecho para
/// romper el lector no cierra el explorador).
core::Result<DocumentPreview> documentPreview(const QString &path, const tasks::CancelToken &cancel,
                                              const PreviewLimits &limits = {});

/// "A", "B", … "Z", "AA"… (1 = A).
QString spreadsheetColumnName(int column);

/// Para pasarlo entre el proceso auxiliar y la aplicación.
QJsonObject documentPreviewToJson(const DocumentPreview &preview);
DocumentPreview documentPreviewFromJson(const QJsonObject &object);

} // namespace eu::content
