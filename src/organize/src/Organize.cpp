// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/organize/Organize.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>

namespace eu::organize {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::organize", text);
}

Rule rule(const QString &name, const QString &extensions, const QString &folder)
{
    Rule made;
    made.name = name;
    made.extensions = extensions.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    made.folder = folder;
    return made;
}

/// Un trozo de ruta que viene de un dato: sin separadores ni caracteres que
/// la unidad no admite; vacío, "Sin dato".
QString cleanSegment(QString segment, core::NameRules rules)
{
    segment.replace(QLatin1Char('\\'), QLatin1Char('-'));
    segment.replace(QLatin1Char('/'), QLatin1Char('-'));
    segment = segment.simplified();
    if (segment.isEmpty()) {
        return tr("Sin dato");
    }
    core::NameCheck check = core::checkFileName(segment, rules);
    for (int guard = 0; !check.ok() && guard < 64; ++guard) {
        if (check.problem == core::NameProblem::InvalidCharacter) {
            segment.replace(check.character, QLatin1Char('_'));
        } else if (check.problem == core::NameProblem::TrailingDotOrSpace) {
            segment.chop(1);
        } else if (check.problem == core::NameProblem::TooLong) {
            segment.truncate(200);
        } else {
            segment = QLatin1Char('_') + segment;   // reservado, "." o ".."
        }
        check = core::checkFileName(segment, rules);
    }
    return segment;
}

QString joinPath(const QString &directory, const QString &name)
{
    return directory.endsWith(QLatin1Char('/')) ? directory + name : directory + QLatin1Char('/') + name;
}

/// Un nombre libre en `folder` que no esté ya reservado por otro movimiento.
QString freeName(const QString &folder, const QString &name, const QSet<QString> &reserved)
{
    const auto taken = [&](const QString &candidate) {
        const QString path = joinPath(folder, candidate);
        return reserved.contains(path.toCaseFolded()) || QFileInfo::exists(path) || QFileInfo(path).isSymLink();
    };
    if (!taken(name)) {
        return name;
    }
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot > 0 ? name.left(dot) : name;
    const QString suffix = dot > 0 ? name.mid(dot) : QString();
    for (int number = 2;; ++number) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(stem).arg(number).arg(suffix);
        if (!taken(candidate)) {
            return candidate;
        }
    }
}

} // namespace

QList<Rule> defaultRules()
{
    return {
        rule(tr("PDF"), QStringLiteral("pdf"), tr("Documentos/PDF")),
        rule(tr("Word"), QStringLiteral("doc docx odt rtf"), tr("Documentos/Word")),
        rule(tr("Excel"), QStringLiteral("xls xlsx ods csv"), tr("Documentos/Excel")),
        rule(tr("PowerPoint"), QStringLiteral("ppt pptx odp"), tr("Documentos/PowerPoint")),
        rule(tr("Texto"), QStringLiteral("txt md"), tr("Documentos/Texto")),
        rule(tr("Páginas web"), QStringLiteral("html htm mhtml"), tr("Documentos/Web")),
        rule(tr("Libros electrónicos"), QStringLiteral("epub mobi azw azw3 kfx fb2 djvu cbz cbr"),
             tr("Documentos/Libros")),
        rule(tr("JPG"), QStringLiteral("jpg jpeg"), tr("Imágenes/JPG")),
        rule(tr("PNG"), QStringLiteral("png"), tr("Imágenes/PNG")),
        rule(tr("RAW"), QStringLiteral("cr2 cr3 nef arw dng orf rw2 raf pef srw"), tr("Imágenes/RAW")),
        rule(tr("Otras imágenes"), QStringLiteral("gif webp heic heif bmp tif tiff svg"), tr("Imágenes/Otras")),
        rule(tr("MP4"), QStringLiteral("mp4 m4v"), tr("Videos/MP4")),
        rule(tr("MKV"), QStringLiteral("mkv"), tr("Videos/MKV")),
        rule(tr("Otros videos"), QStringLiteral("avi mov wmv webm mpg mpeg 3gp ts"), tr("Videos/Otros")),
        rule(tr("MP3"), QStringLiteral("mp3"), tr("Audio/MP3")),
        rule(tr("FLAC"), QStringLiteral("flac"), tr("Audio/FLAC")),
        rule(tr("Otro audio"), QStringLiteral("ogg opus m4a aac wav wma"), tr("Audio/Otros")),
        rule(tr("Comprimidos"), QStringLiteral("zip 7z rar tar gz bz2 xz tgz"), tr("Comprimidos")),
    };
}

QList<QPair<QString, QList<Rule>>> presets()
{
    const QString photos = QStringLiteral("jpg jpeg png heic heif cr2 cr3 nef arw dng orf rw2 raf");
    return {
        {tr("Por tipo (predeterminado)"), defaultRules()},
        {tr("Por fecha: año y mes"), {rule(tr("Todo"), {}, QStringLiteral("{fecha:yyyy}/{fecha:MM}"))}},
        {tr("Fotos por fecha de la toma"),
         {rule(tr("Fotos"), photos, QStringLiteral("{fecha_foto:yyyy}/{fecha_foto:yyyy-MM}"))}},
        {tr("Por tamaño"), {rule(tr("Todo"), {}, QStringLiteral("{tamaño}"))}},
        {tr("Música por artista y álbum"),
         {rule(tr("Música"), QStringLiteral("mp3 flac ogg opus m4a aac wav wma"), QStringLiteral("{artista}/{album}"))}},
    };
}

Plan simulate(const QString &root, const Options &options, const tasks::CancelToken &cancel,
              const PlanOptions &planOptions)
{
    Plan plan;
    const QString base = QDir::cleanPath(root);
    const core::NameRules rules = planOptions.rulesFor ? planOptions.rulesFor(base) : core::nativeNameRules();

    // Los archivos, en orden estable (para que los "(2)" salgan siempre igual).
    QStringList files;
    QDir::Filters filters = QDir::Files | QDir::NoDotAndDotDot;
    if (options.includeHidden) {
        filters |= QDir::Hidden;
    }
    QDirIterator it(base, filters, options.recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
    while (it.hasNext() && !cancel.isCancelled()) {
        const QString path = it.next();
        if (!QFileInfo(path).isSymLink()) {
            files.append(path);
        }
    }
    if (!options.recursive) {
        // Lo que hay en subcarpetas no se mira: se cuenta para decirlo.
        QDirIterator inside(base, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::NoIteratorFlags);
        while (inside.hasNext() && !cancel.isCancelled() && plan.inSubfolders < 100000) {
            QDirIterator nested(inside.next(), filters, QDirIterator::Subdirectories);
            while (nested.hasNext() && plan.inSubfolders < 100000) {
                nested.next();
                ++plan.inSubfolders;
            }
        }
    }
    files.sort();

    QSet<QString> reserved;   // destinos ya elegidos, en minúsculas
    QSet<QString> folders;
    int index = 0;
    for (const QString &path : std::as_const(files)) {
        if (cancel.isCancelled()) {
            break;
        }
        const QFileInfo info(path);
        const QString extension = info.suffix().toLower();
        const Rule *matched = nullptr;
        for (const Rule &candidate : options.rules) {
            if (!candidate.extensions.isEmpty() && !candidate.extensions.contains(extension)) {
                continue;
            }
            if ((candidate.minSize >= 0 && info.size() < candidate.minSize)
                || (candidate.maxSize >= 0 && info.size() > candidate.maxSize)) {
                continue;
            }
            matched = &candidate;
            break;
        }
        if (matched == nullptr) {
            ++plan.withoutRule;
            continue;
        }
        rename::Item item;
        item.path = path;
        item.modifiedMs = info.lastModified().toMSecsSinceEpoch();
        item.createdMs = info.birthTime().isValid() ? info.birthTime().toMSecsSinceEpoch() : 0;
        item.size = info.size();
        const rename::Metadata metadata = planOptions.metadataFor && rename::templateUsesMetadata(matched->folder)
            ? planOptions.metadataFor(path)
            : rename::Metadata();
        // La subcarpeta, trozo a trozo: un dato con "/" no crea carpetas de más.
        QString folder = base;
        for (const QString &segment : matched->folder.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
            folder = joinPath(folder, cleanSegment(rename::expandTemplate(segment, item, index, metadata), rules));
        }
        ++index;
        if (QDir::cleanPath(info.absolutePath()) == QDir::cleanPath(folder)) {
            ++plan.alreadyInPlace;
            continue;
        }
        Move move;
        move.source = path;
        move.ruleName = matched->name;
        const QString name = freeName(folder, info.fileName(), reserved);
        if (name != info.fileName()) {
            move.note = tr("Se llamará «%1»: ya hay otro «%2» allí").arg(name, info.fileName());
        }
        move.target = joinPath(folder, name);
        reserved.insert(move.target.toCaseFolded());
        plan.moves.append(move);
        // Las carpetas que faltan, de fuera hacia dentro.
        QStringList chain;
        for (QString current = folder; current != base && current.startsWith(base) && !QFileInfo(current).isDir();
             current = QFileInfo(current).absolutePath()) {
            chain.prepend(current);
        }
        for (const QString &missing : std::as_const(chain)) {
            if (!folders.contains(missing)) {
                folders.insert(missing);
                plan.foldersToCreate.append(missing);
            }
        }
    }
    std::sort(plan.foldersToCreate.begin(), plan.foldersToCreate.end());
    return plan;
}

QByteArray rulesToJson(const QList<Rule> &rules)
{
    QJsonArray array;
    for (const Rule &rule : rules) {
        array.append(QJsonObject{{QStringLiteral("nombre"), rule.name},
                                 {QStringLiteral("extensiones"), rule.extensions.join(QLatin1Char(' '))},
                                 {QStringLiteral("minimo"), rule.minSize},
                                 {QStringLiteral("maximo"), rule.maxSize},
                                 {QStringLiteral("carpeta"), rule.folder}});
    }
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

QList<Rule> rulesFromJson(const QByteArray &json)
{
    QList<Rule> rules;
    for (const QJsonValue &value : QJsonDocument::fromJson(json).array()) {
        const QJsonObject object = value.toObject();
        Rule made;
        made.name = object.value(QStringLiteral("nombre")).toString();
        made.extensions = object.value(QStringLiteral("extensiones")).toString().toLower().split(QLatin1Char(' '),
                                                                                                    Qt::SkipEmptyParts);
        made.minSize = object.value(QStringLiteral("minimo")).toInteger(-1);
        made.maxSize = object.value(QStringLiteral("maximo")).toInteger(-1);
        made.folder = object.value(QStringLiteral("carpeta")).toString();
        if (!made.folder.isEmpty()) {
            rules.append(made);
        }
    }
    return rules;
}

} // namespace eu::organize
