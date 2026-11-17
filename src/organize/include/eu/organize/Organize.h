// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/core/FileName.h"
#include "eu/rename/Rename.h"
#include "eu/tasks/Cancel.h"

#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace eu::organize {

/// Una regla: qué archivos (por extensión y tamaño) y a qué subcarpeta van.
/// La subcarpeta admite los datos del renombrado: "Fotos/{fecha_foto:yyyy}/{fecha_foto:MM}",
/// "Música/{artista}/{album}", "{tamaño}"… Gana la primera regla que encaja.
struct Rule
{
    QString name;              ///< Para la ventana ("PDF", "Fotos por año"…).
    QStringList extensions;    ///< En minúsculas, sin punto. Vacía: cualquiera.
    qint64 minSize = -1;       ///< Bytes; -1 sin límite.
    qint64 maxSize = -1;
    QString folder;            ///< Subcarpeta de destino, relativa a la carpeta que se organiza.
};

struct Options
{
    QList<Rule> rules;
    bool recursive = false;    ///< También lo de las subcarpetas (SPEC §12: se puede no recorrerlas).
    bool includeHidden = false;
};

/// Las reglas de fábrica (SPEC §12): Documentos/PDF, Documentos/Word,
/// Imágenes/JPG, Imágenes/RAW, Videos/MP4, Audio/MP3…
QList<Rule> defaultRules();
/// Otros conjuntos listos: por fecha (año/mes), por fecha de la foto, por
/// tamaño, música por artista y álbum.
QList<QPair<QString, QList<Rule>>> presets();

/// Un movimiento de la simulación.
struct Move
{
    QString source;
    QString target;       ///< Ruta final completa.
    QString ruleName;
    QString note;         ///< "Renombrado: ya había otro con ese nombre"…
};

struct Plan
{
    QList<Move> moves;
    QStringList foldersToCreate;   ///< Rutas completas, de fuera hacia dentro.
    int alreadyInPlace = 0;        ///< Ya estaban donde les toca.
    int withoutRule = 0;           ///< Ninguna regla los cubre: se quedan.
    int inSubfolders = 0;          ///< Sin recorrer subcarpetas: archivos que hay en ellas y no se miran.
};

struct PlanOptions
{
    std::function<core::NameRules(const QString &directory)> rulesFor;
    std::function<rename::Metadata(const QString &path)> metadataFor;
};

/// La simulación completa (SPEC §12): qué se mueve a dónde y qué carpetas se
/// crean. Los choques de nombre se resuelven ya aquí ("foto (2).jpg") para
/// que la simulación sea exactamente lo que se hará. No toca nada; lee el
/// disco: se llama desde una tarea.
Plan simulate(const QString &root, const Options &options, const tasks::CancelToken &cancel,
              const PlanOptions &planOptions = {});

/// Reglas guardadas (JSON) y de vuelta.
QByteArray rulesToJson(const QList<Rule> &rules);
QList<Rule> rulesFromJson(const QByteArray &json);

} // namespace eu::organize
