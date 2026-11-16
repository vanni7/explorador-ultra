// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/rename/Rename.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace eu::rename {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("eu::rename", text);
}

/// Datos que salen de los metadatos del archivo (los demás, del propio archivo).
const QSet<QString> &metadataKeys()
{
    static const QSet<QString> keys{QStringLiteral("fecha_foto"), QStringLiteral("camara"), QStringLiteral("artista"),
                                    QStringLiteral("album"),       QStringLiteral("pista"),  QStringLiteral("titulo"),
                                    QStringLiteral("año"),         QStringLiteral("genero"), QStringLiteral("ancho"),
                                    QStringLiteral("alto")};
    return keys;
}

const QRegularExpression &tokenPattern()
{
    static const QRegularExpression pattern(QStringLiteral("\\{([^{}:]+)(?::([^{}]*))?\\}"));
    return pattern;
}

struct Parts
{
    QString stem;        ///< El nombre sin la extensión (o entero, según el modo).
    QString extension;   ///< Sin el punto.
};

Parts split(const Item &item, ExtensionMode mode)
{
    const QString name = QFileInfo(item.path).fileName();
    if (item.isDirectory || mode == ExtensionMode::Include) {
        return {name, {}};
    }
    // "foto.jpg" -> "foto" + "jpg"; ".bashrc" no tiene extensión.
    const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0) {
        return {name, {}};
    }
    return {name.left(dot), name.mid(dot + 1)};
}

QString formatDate(qint64 ms, const QString &format)
{
    if (ms <= 0) {
        return {};
    }
    return QDateTime::fromMSecsSinceEpoch(ms).toString(format.isEmpty() ? QStringLiteral("yyyy-MM-dd") : format);
}

/// Sustituye los {datos} de una plantilla.
QString expand(const QString &text, const Item &item, int index, const Metadata &metadata)
{
    if (!text.contains(QLatin1Char('{'))) {
        return text;
    }
    const Parts original = split(item, ExtensionMode::Keep);
    QString out;
    qsizetype last = 0;
    QRegularExpressionMatchIterator it = tokenPattern().globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out += text.mid(last, match.capturedStart() - last);
        last = match.capturedEnd();
        const QString key = match.captured(1).trimmed().toLower();
        const QString argument = match.captured(2);
        QString value;
        bool known = true;
        if (key == QLatin1String("nombre")) {
            value = original.stem;
        } else if (key == QLatin1String("ext")) {
            value = original.extension;
        } else if (key == QLatin1String("carpeta")) {
            value = QFileInfo(QFileInfo(item.path).absolutePath()).fileName();
        } else if (key == QLatin1String("contador")) {
            value = QString::number(index + 1).rightJustified(std::max(1, argument.toInt()), QLatin1Char('0'));
        } else if (key == QLatin1String("fecha")) {
            value = formatDate(item.modifiedMs, argument);
        } else if (key == QLatin1String("creado")) {
            value = formatDate(item.createdMs > 0 ? item.createdMs : item.modifiedMs, argument);
        } else if (key == QLatin1String("fecha_foto")) {
            // "2024-03-12 16:57:40"; sin ella, la de modificación.
            const QDateTime taken = QDateTime::fromString(metadata.value(key), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
            value = taken.isValid() ? taken.toString(argument.isEmpty() ? QStringLiteral("yyyy-MM-dd") : argument)
                                    : formatDate(item.modifiedMs, argument);
        } else if (key == QLatin1String("pista")) {
            // "3/12" -> "03" con {pista:2}.
            const QString number = metadata.value(key).section(QLatin1Char('/'), 0, 0).trimmed();
            value = argument.isEmpty() ? number : number.rightJustified(argument.toInt(), QLatin1Char('0'));
        } else if (metadataKeys().contains(key)) {
            value = metadata.value(key);
        } else {
            known = false;
        }
        out += known ? value : match.captured(0);   // un dato desconocido se deja tal cual
    }
    out += text.mid(last);
    return out;
}

QString titleCase(const QString &text)
{
    QString out = text.toLower();
    bool wordStart = true;
    for (QChar &character : out) {
        if (character.isLetterOrNumber()) {
            if (wordStart) {
                character = character.toUpper();
            }
            wordStart = false;
        } else {
            wordStart = character != QLatin1Char('\'');   // "l'avenir": no "L'Avenir"
        }
    }
    return out;
}

QString sentenceCase(const QString &text)
{
    QString out = text.toLower();
    for (QChar &character : out) {
        if (character.isLetter()) {
            character = character.toUpper();
            break;
        }
    }
    return out;
}

/// "$1" y "\1" valen como referencia en la sustitución.
QString regexReplacement(const QString &with)
{
    QString out;
    for (qsizetype i = 0; i < with.size(); ++i) {
        if (with[i] == QLatin1Char('$') && i + 1 < with.size() && with[i + 1].isDigit()) {
            out += QLatin1Char('\\');   // "$1" -> "\1", que es lo que entiende Qt
            continue;
        }
        out += with[i];
    }
    return out;
}

QString applyRule(const Rule &rule, const QString &current, int index, const Item &item, const Metadata &metadata)
{
    const Qt::CaseSensitivity sensitivity = rule.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    switch (rule.kind) {
    case Rule::Kind::Case:
        switch (rule.caseMode) {
        case Rule::CaseMode::Upper:    return current.toUpper();
        case Rule::CaseMode::Lower:    return current.toLower();
        case Rule::CaseMode::Title:    return titleCase(current);
        case Rule::CaseMode::Sentence: return sentenceCase(current);
        }
        return current;
    case Rule::Kind::Replace:
        return rule.find.isEmpty() ? current : QString(current).replace(rule.find, rule.with, sensitivity);
    case Rule::Kind::Remove: {
        QString out = current;
        switch (rule.removeMode) {
        case Rule::RemoveMode::Characters:
            for (const QChar character : rule.characters) {
                out.remove(character, sensitivity);
            }
            return out;
        case Rule::RemoveMode::First:
            return out.mid(std::max(0, rule.count));
        case Rule::RemoveMode::Last:
            out.chop(std::max(0, rule.count));
            return out;
        case Rule::RemoveMode::Range:
            return out.remove(std::clamp<qsizetype>(rule.from, 0, out.size()), std::max(0, rule.count));
        }
        return out;
    }
    case Rule::Kind::Insert: {
        const QString text = expand(rule.with, item, index, metadata);
        switch (rule.position) {
        case Rule::Position::Start: return text + current;
        case Rule::Position::End:   return current + text;
        case Rule::Position::At:
            return QString(current).insert(std::clamp<qsizetype>(rule.from, 0, current.size()), text);
        }
        return current;
    }
    case Rule::Kind::Regex: {
        if (rule.find.isEmpty()) {
            return current;
        }
        const QRegularExpression pattern(rule.find, rule.caseSensitive ? QRegularExpression::NoPatternOption
                                                                       : QRegularExpression::CaseInsensitiveOption);
        if (!pattern.isValid()) {
            return current;
        }
        return QString(current).replace(pattern, regexReplacement(rule.with));
    }
    case Rule::Kind::Counter: {
        const QString number = QString::number(qint64(rule.start) + qint64(index) * rule.step)
                                   .rightJustified(std::max(1, rule.count), QLatin1Char('0'));
        return rule.position == Rule::Position::End ? current + rule.separator + number
                                                    : number + rule.separator + current;
    }
    case Rule::Kind::Template:
        // Un dato que no hay deja espacios de más ("2024-03-12  .jpg"): se
        // juntan y se quitan los de los extremos.
        return expand(rule.with, item, index, metadata).simplified();
    }
    return current;
}

bool usesMetadata(const Options &options)
{
    for (const Rule &rule : options.rules) {
        if (rule.kind != Rule::Kind::Template && rule.kind != Rule::Kind::Insert) {
            continue;
        }
        QRegularExpressionMatchIterator it = tokenPattern().globalMatch(rule.with);
        while (it.hasNext()) {
            if (metadataKeys().contains(it.next().captured(1).trimmed().toLower())) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

QList<QPair<QString, QString>> templateTokens()
{
    return {
        {QStringLiteral("{nombre}"), tr("El nombre actual, sin la extensión")},
        {QStringLiteral("{ext}"), tr("La extensión actual")},
        {QStringLiteral("{carpeta}"), tr("La carpeta donde está")},
        {QStringLiteral("{contador:3}"), tr("1, 2, 3… con 3 cifras: 001, 002…")},
        {QStringLiteral("{fecha:yyyy-MM-dd}"), tr("Fecha de modificación (yyyy año, MM mes, dd día, HH.mm hora)")},
        {QStringLiteral("{creado:yyyy-MM-dd}"), tr("Fecha de creación")},
        {QStringLiteral("{fecha_foto:yyyy-MM-dd HH.mm.ss}"), tr("Fecha de la toma (EXIF); si no hay, la de modificación")},
        {QStringLiteral("{camara}"), tr("Cámara (EXIF)")},
        {QStringLiteral("{ancho}"), tr("Ancho en píxeles")},
        {QStringLiteral("{alto}"), tr("Alto en píxeles")},
        {QStringLiteral("{artista}"), tr("Artista (etiquetas de audio)")},
        {QStringLiteral("{album}"), tr("Álbum")},
        {QStringLiteral("{pista:2}"), tr("Número de pista, con 2 cifras")},
        {QStringLiteral("{titulo}"), tr("Título")},
        {QStringLiteral("{año}"), tr("Año")},
        {QStringLiteral("{genero}"), tr("Género")},
    };
}

QString proposeName(const Item &item, int index, const Options &options, const Metadata &metadata)
{
    const Parts parts = split(item, options.extension);
    QString stem = parts.stem;
    for (const Rule &rule : options.rules) {
        stem = applyRule(rule, stem, index, item, metadata);
    }
    QString extension = parts.extension;
    if (options.extension == ExtensionMode::Lower) {
        extension = extension.toLower();
    } else if (options.extension == ExtensionMode::Upper) {
        extension = extension.toUpper();
    }
    return extension.isEmpty() ? stem : stem + QLatin1Char('.') + extension;
}

QList<Proposal> plan(const QList<Item> &items, const Options &options, const PlanOptions &planOptions)
{
    QList<Proposal> proposals;
    // Una expresión regular mal escrita invalida toda la vista previa.
    QString ruleProblem;
    for (const Rule &rule : options.rules) {
        if (rule.kind == Rule::Kind::Regex && !rule.find.isEmpty()) {
            const QRegularExpression pattern(rule.find);
            if (!pattern.isValid()) {
                ruleProblem = tr("Expresión regular no válida: %1").arg(pattern.errorString());
            }
        }
    }
    const bool withMetadata = planOptions.metadataFor && usesMetadata(options);
    for (int index = 0; index < items.size(); ++index) {
        const Item &item = items[index];
        Proposal proposal;
        proposal.path = QDir::cleanPath(item.path);
        proposal.oldName = QFileInfo(proposal.path).fileName();
        proposal.newName = proposeName(item, index, options, withMetadata ? planOptions.metadataFor(item) : Metadata());
        const QString directory = QFileInfo(proposal.path).absolutePath();
        const core::NameCheck check = core::checkFileName(
            proposal.newName, planOptions.rulesFor ? planOptions.rulesFor(directory) : core::nativeNameRules());
        if (!ruleProblem.isEmpty()) {
            proposal.status = Proposal::Status::Invalid;
            proposal.problem = ruleProblem;
        } else if (proposal.newName == proposal.oldName) {
            proposal.status = Proposal::Status::Unchanged;
        } else if (!check.ok()) {
            proposal.status = Proposal::Status::Invalid;
            proposal.problem = core::describeNameProblem(check);
        } else if (!QFileInfo(directory).isWritable()) {
            proposal.status = Proposal::Status::Protected;
            proposal.problem = tr("No se puede escribir en «%1»").arg(QDir::toNativeSeparators(directory));
        }
        proposals.append(proposal);
    }

    // Conflictos, carpeta por carpeta. Un nombre está ocupado si lo tiene algo
    // del disco que no está en la lista, o un elemento de la lista que se
    // queda con su nombre (sin cambios o porque no se puede renombrar). Se
    // repite hasta que nada cambia: un conflicto deja ocupado su nombre viejo.
    QHash<QString, QList<int>> byDirectory;
    for (int index = 0; index < proposals.size(); ++index) {
        byDirectory[QFileInfo(proposals[index].path).absolutePath()].append(index);
    }
    for (auto group = byDirectory.constBegin(); group != byDirectory.constEnd(); ++group) {
        QSet<QString> inList;
        for (const int index : group.value()) {
            inList.insert(proposals[index].oldName);
        }
        QHash<QString, QString> onDisk;   // en minúsculas -> nombre real, de lo que no está en la lista
        const QStringList entries = QDir(group.key()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System
                                                                | QDir::NoDotAndDotDot);
        for (const QString &name : entries) {
            if (!inList.contains(name)) {
                onDisk.insert(name.toCaseFolded(), name);
            }
        }
        bool changed = true;
        while (changed) {
            changed = false;
            QHash<QString, QString> occupied = onDisk;    // en minúsculas -> nombre
            QHash<QString, QList<int>> wanted;             // en minúsculas -> quién lo quiere
            for (const int index : group.value()) {
                const Proposal &proposal = proposals[index];
                if (proposal.willRename()) {
                    wanted[proposal.newName.toCaseFolded()].append(index);
                } else {
                    occupied.insert(proposal.oldName.toCaseFolded(), proposal.oldName);
                }
            }
            for (auto want = wanted.constBegin(); want != wanted.constEnd(); ++want) {
                const QList<int> &who = want.value();
                const auto taken = occupied.constFind(want.key());
                for (const int index : who) {
                    Proposal &proposal = proposals[index];
                    QString problem;
                    if (taken != occupied.constEnd()) {
                        problem = taken.value() == proposal.newName
                            ? tr("Ya existe «%1» en la carpeta").arg(taken.value())
                            : tr("Choca con «%1»: solo cambian mayúsculas y minúsculas, y en Windows y macOS es el "
                                 "mismo nombre").arg(taken.value());
                    } else if (who.size() > 1) {
                        const QString &other = proposals[who.first() == index ? who[1] : who.first()].newName;
                        problem = other == proposal.newName
                            ? tr("Otro elemento también acabaría llamándose «%1»").arg(other)
                            : tr("Otro acabaría llamándose «%1»: solo cambian mayúsculas y minúsculas").arg(other);
                    }
                    if (!problem.isEmpty()) {
                        proposal.status = Proposal::Status::Conflict;
                        proposal.problem = problem;
                        changed = true;
                    }
                }
            }
        }
    }
    return proposals;
}

} // namespace eu::rename
