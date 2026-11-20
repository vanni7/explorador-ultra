// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/sync/Sync.h"

#include "eu/journal/Journal.h"
#include "eu/ops/Engine.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

using namespace eu;
using namespace eu::sync;

namespace {

/// Escribe un archivo y le pone la fecha `ms` (0: la de ahora).
void writeFile(const QString &path, const QByteArray &data, qint64 ms = 0)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(data);
    file.flush();   // si no, lo que queda en el búfer se escribe al cerrar y cambia la fecha
    if (ms > 0) {
        QVERIFY(file.setFileTime(QDateTime::fromMSecsSinceEpoch(ms), QFileDevice::FileModificationTime));
    }
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("(no existe)");
}

const Item *itemOf(const Comparison &comparison, const QString &relativePath)
{
    for (const Item &item : comparison.items) {
        if (item.relativePath == relativePath) {
            return &item;
        }
    }
    return nullptr;
}

const Action *actionOf(const Plan &plan, const QString &relativePath)
{
    for (const Action &action : plan.actions) {
        if (action.relativePath == relativePath) {
            return &action;
        }
    }
    return nullptr;
}

constexpr qint64 base = 1'700'000'000'000;   // una fecha fija (nov. 2023)
constexpr qint64 hour = 3'600'000;

} // namespace

class TestSync : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString m_left;
    QString m_right;
    const tasks::CancelToken m_noCancel;

    /// Dos árboles con un poco de todo.
    void makeTrees()
    {
        m_left = m_dir.filePath(QStringLiteral("izquierda"));
        m_right = m_dir.filePath(QStringLiteral("derecha"));
        QDir(m_left).removeRecursively();
        QDir(m_right).removeRecursively();
        writeFile(m_left + QStringLiteral("/igual.txt"), "igual", base);
        writeFile(m_right + QStringLiteral("/igual.txt"), "igual", base);
        writeFile(m_left + QStringLiteral("/cambiado.txt"), "nuevo texto", base + hour);
        writeFile(m_right + QStringLiteral("/cambiado.txt"), "viejo", base);
        writeFile(m_left + QStringLiteral("/viejo.txt"), "aaa", base);
        writeFile(m_right + QStringLiteral("/viejo.txt"), "bbbb", base + hour);
        writeFile(m_left + QStringLiteral("/mismo contenido.txt"), "xyz", base);
        writeFile(m_right + QStringLiteral("/mismo contenido.txt"), "xyz", base + hour);
        writeFile(m_left + QStringLiteral("/solo izquierda.txt"), "L", base);
        writeFile(m_right + QStringLiteral("/solo derecha.txt"), "R", base);
        writeFile(m_left + QStringLiteral("/carpeta nueva/a.txt"), "12345", base);
        writeFile(m_left + QStringLiteral("/carpeta nueva/sub/b.txt"), "678", base);
        writeFile(m_left + QStringLiteral("/común/dentro.txt"), "uno", base + hour);
        writeFile(m_right + QStringLiteral("/común/dentro.txt"), "dos", base);
        writeFile(m_left + QStringLiteral("/choque"), "soy archivo", base);
        QVERIFY(QDir().mkpath(m_right + QStringLiteral("/choque")));
        writeFile(m_left + QStringLiteral("/.oculto"), "no se mira", base);
#ifdef Q_OS_WIN
        // En Windows lo oculto es un atributo, no el punto delante del nombre.
        SetFileAttributesW(reinterpret_cast<const wchar_t *>(
                               QDir::toNativeSeparators(m_left + QStringLiteral("/.oculto")).utf16()),
                           FILE_ATTRIBUTE_HIDDEN);
#endif
    }

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QVERIFY(m_dir.isValid());
    }

    void compareStatesAndCriteria()
    {
        makeTrees();
        Comparison comparison = compare(m_left, m_right, {}, m_noCancel);
        QVERIFY(comparison.errors.isEmpty());
        QCOMPARE(itemOf(comparison, QStringLiteral("igual.txt"))->state, State::Identical);
        QCOMPARE(itemOf(comparison, QStringLiteral("cambiado.txt"))->state, State::LeftNewer);
        QCOMPARE(itemOf(comparison, QStringLiteral("viejo.txt"))->state, State::RightNewer);
        // Por tamaño y fecha, el mismo contenido con otra fecha es distinto.
        QCOMPARE(itemOf(comparison, QStringLiteral("mismo contenido.txt"))->state, State::RightNewer);
        QCOMPARE(itemOf(comparison, QStringLiteral("solo izquierda.txt"))->state, State::LeftOnly);
        QCOMPARE(itemOf(comparison, QStringLiteral("solo derecha.txt"))->state, State::RightOnly);
        QCOMPARE(itemOf(comparison, QStringLiteral("choque"))->state, State::TypeConflict);
        // Una carpeta que solo está en un lado sale una vez, con lo que suma.
        const Item *folder = itemOf(comparison, QStringLiteral("carpeta nueva"));
        QVERIFY(folder != nullptr);
        QCOMPARE(folder->state, State::LeftOnly);
        QCOMPARE(folder->left.size, qint64(8));
        QCOMPARE(itemOf(comparison, QStringLiteral("carpeta nueva/a.txt")), nullptr);
        // La que está en los dos, con su contenido y la marca de que algo difiere.
        QVERIFY(itemOf(comparison, QStringLiteral("común"))->differsInside);
        QCOMPARE(itemOf(comparison, QStringLiteral("común/dentro.txt"))->state, State::LeftNewer);
        QCOMPARE(itemOf(comparison, QStringLiteral(".oculto")), nullptr);
        QVERIFY(!comparison.sameTrees());

        // Con hash o byte a byte, el mismo contenido es idéntico aunque cambie la fecha.
        CompareOptions byHash;
        byHash.criterion = Criterion::Hash;
        comparison = compare(m_left, m_right, byHash, m_noCancel);
        QCOMPARE(itemOf(comparison, QStringLiteral("mismo contenido.txt"))->state, State::Identical);
        QCOMPARE(itemOf(comparison, QStringLiteral("cambiado.txt"))->state, State::LeftNewer);
        CompareOptions byBytes;
        byBytes.criterion = Criterion::Content;
        byBytes.includeHidden = true;
        comparison = compare(m_left, m_right, byBytes, m_noCancel);
        QCOMPARE(itemOf(comparison, QStringLiteral("mismo contenido.txt"))->state, State::Identical);
        QCOMPARE(itemOf(comparison, QStringLiteral(".oculto"))->state, State::LeftOnly);
        // Solo por nombre, todo lo que está en los dos es idéntico.
        CompareOptions byName;
        byName.criterion = Criterion::Name;
        comparison = compare(m_left, m_right, byName, m_noCancel);
        QCOMPARE(itemOf(comparison, QStringLiteral("viejo.txt"))->state, State::Identical);

        // Dos árboles iguales.
        const Comparison same = compare(m_left, m_left, {}, m_noCancel);
        QVERIFY(same.sameTrees());
    }

    void oneWayPlans()
    {
        makeTrees();
        const Comparison comparison = compare(m_left, m_right, {}, m_noCancel);
        SyncOptions options;   // izquierda → derecha, sin borrar, sin pisar lo más reciente
        Plan plan = sync::plan(comparison, options, nullptr);
        QCOMPARE(actionOf(plan, QStringLiteral("cambiado.txt"))->kind, ActionKind::CopyToRight);
        QVERIFY(actionOf(plan, QStringLiteral("cambiado.txt"))->replaces);
        QCOMPARE(actionOf(plan, QStringLiteral("solo izquierda.txt"))->kind, ActionKind::CopyToRight);
        QCOMPARE(actionOf(plan, QStringLiteral("carpeta nueva"))->kind, ActionKind::CopyToRight);
        QCOMPARE(actionOf(plan, QStringLiteral("común/dentro.txt"))->kind, ActionKind::CopyToRight);
        // Lo más reciente en el destino no se pisa: conflicto.
        QCOMPARE(actionOf(plan, QStringLiteral("viejo.txt"))->kind, ActionKind::Skip);
        QVERIFY(actionOf(plan, QStringLiteral("viejo.txt"))->conflict);
        // Sin política de borrado, lo que solo está en el destino se queda.
        QCOMPARE(actionOf(plan, QStringLiteral("solo derecha.txt"))->kind, ActionKind::Skip);
        QCOMPARE(plan.deletionsNotPropagated, 1);
        QCOMPARE(actionOf(plan, QStringLiteral("choque"))->kind, ActionKind::Skip);
        QCOMPARE(actionOf(plan, QStringLiteral("igual.txt")), nullptr);
        QCOMPARE(plan.conflicts, 3);   // viejo, mismo contenido (destino más reciente) y choque
        QCOMPARE(plan.bytesToCopy, qint64(11 + 1 + 8 + 3));

        // Espejo: manda el origen y se quita lo que sobra (a la papelera).
        options.conflicts = ConflictPolicy::LeftWins;
        options.propagateDeletions = true;
        plan = sync::plan(comparison, options, nullptr);
        QCOMPARE(actionOf(plan, QStringLiteral("viejo.txt"))->kind, ActionKind::CopyToRight);
        QCOMPARE(actionOf(plan, QStringLiteral("solo derecha.txt"))->kind, ActionKind::TrashRight);
        QCOMPARE(plan.conflicts, 1);   // el choque archivo/carpeta nunca se resuelve solo

        // Al revés: de derecha a izquierda.
        options = SyncOptions();
        options.direction = Direction::RightToLeft;
        plan = sync::plan(comparison, options, nullptr);
        QCOMPARE(actionOf(plan, QStringLiteral("viejo.txt"))->kind, ActionKind::CopyToLeft);
        QCOMPARE(actionOf(plan, QStringLiteral("solo derecha.txt"))->kind, ActionKind::CopyToLeft);
        QCOMPARE(actionOf(plan, QStringLiteral("cambiado.txt"))->kind, ActionKind::Skip);
        QCOMPARE(actionOf(plan, QStringLiteral("solo izquierda.txt"))->kind, ActionKind::Skip);
    }

    void twoWayPlansUseThePreviousState()
    {
        makeTrees();
        Comparison comparison = compare(m_left, m_right, {}, m_noCancel);
        SyncOptions options;
        options.direction = Direction::Both;
        // Primera vez: lo de un solo lado se copia al otro; lo distinto es conflicto.
        Plan plan = sync::plan(comparison, options, nullptr);
        QCOMPARE(actionOf(plan, QStringLiteral("solo izquierda.txt"))->kind, ActionKind::CopyToRight);
        QCOMPARE(actionOf(plan, QStringLiteral("solo derecha.txt"))->kind, ActionKind::CopyToLeft);
        QCOMPARE(actionOf(plan, QStringLiteral("cambiado.txt"))->kind, ActionKind::Skip);
        QVERIFY(actionOf(plan, QStringLiteral("cambiado.txt"))->conflict);
        // «Gana el más reciente» los resuelve.
        options.conflicts = ConflictPolicy::NewerWins;
        plan = sync::plan(comparison, options, nullptr);
        QCOMPARE(actionOf(plan, QStringLiteral("cambiado.txt"))->kind, ActionKind::CopyToRight);
        QCOMPARE(actionOf(plan, QStringLiteral("viejo.txt"))->kind, ActionKind::CopyToLeft);

        // Con estado anterior: los dos lados estaban iguales; luego cambia uno
        // y en el otro se borra algo.
        const QString l = m_dir.filePath(QStringLiteral("L"));
        const QString r = m_dir.filePath(QStringLiteral("R"));
        QDir(l).removeRecursively();
        QDir(r).removeRecursively();
        for (const QString &root : {l, r}) {
            writeFile(root + QStringLiteral("/a.txt"), "a", base);
            writeFile(root + QStringLiteral("/b.txt"), "b", base);
            writeFile(root + QStringLiteral("/c.txt"), "c", base);
        }
        const SyncState before = stateFrom(compare(l, r, {}, m_noCancel), {}, nullptr);
        QCOMPARE(before.entries.size(), 3);
        writeFile(l + QStringLiteral("/a.txt"), "a cambiado", base + hour);   // cambia a la izquierda
        QVERIFY(QFile::remove(r + QStringLiteral("/b.txt")));                  // se borra a la derecha
        writeFile(l + QStringLiteral("/c.txt"), "c izq", base + hour);         // cambia en los dos
        writeFile(r + QStringLiteral("/c.txt"), "c der", base + 2 * hour);
        comparison = compare(l, r, {}, m_noCancel);
        options = SyncOptions();
        options.direction = Direction::Both;
        plan = sync::plan(comparison, options, &before);
        QCOMPARE(actionOf(plan, QStringLiteral("a.txt"))->kind, ActionKind::CopyToRight);
        QCOMPARE(actionOf(plan, QStringLiteral("b.txt"))->kind, ActionKind::Skip);   // el borrado no se propaga
        QVERIFY(actionOf(plan, QStringLiteral("b.txt"))->deletionKept);
        QCOMPARE(plan.deletionsNotPropagated, 1);
        QCOMPARE(actionOf(plan, QStringLiteral("c.txt"))->kind, ActionKind::Skip);
        QVERIFY(actionOf(plan, QStringLiteral("c.txt"))->conflict);
        // El estado nuevo recuerda b.txt (si no, la próxima vez se copiaría de vuelta).
        const SyncState after = stateFrom(comparison, plan, &before);
        QVERIFY(after.entries.contains(QStringLiteral("b.txt")));
        QVERIFY(!after.entries.contains(QStringLiteral("c.txt")));
        const Plan again = sync::plan(comparison, options, &after);
        QCOMPARE(actionOf(again, QStringLiteral("b.txt"))->kind, ActionKind::Skip);
        // Con la política de borrado, va a la papelera.
        options.propagateDeletions = true;
        plan = sync::plan(comparison, options, &before);
        QCOMPARE(actionOf(plan, QStringLiteral("b.txt"))->kind, ActionKind::TrashLeft);

        // El estado se guarda y se lee igual.
        const QString file = stateFile(m_dir.path(), l, r);
        QVERIFY(file.contains(QStringLiteral("/sincronizacion/")));
        QVERIFY(saveState(file, after).ok());
        const auto loaded = loadState(file);
        QVERIFY(loaded.ok());
        QCOMPARE(loaded.value().entries.size(), after.entries.size());
        QCOMPARE(loaded.value().entries.value(QStringLiteral("b.txt")).size, qint64(1));
        QVERIFY(loadState(m_dir.filePath(QStringLiteral("no-existe.json"))).value().isEmpty());
    }

    void engineAppliesAPlanAsOneUndoableOperation()
    {
        QTemporaryDir journalDirectory;
        auto journal = journal::Journal::open(journalDirectory.filePath(QStringLiteral("h.db"))).value();
        makeTrees();
        const Comparison comparison = compare(m_left, m_right, {}, m_noCancel);
        SyncOptions options;
        options.conflicts = ConflictPolicy::LeftWins;
        options.propagateDeletions = true;
        const Plan plan = sync::plan(comparison, options, nullptr);
        QList<QPair<QString, QString>> copies;
        QStringList toTrash;
        for (const Action &action : plan.actions) {
            if (action.kind == ActionKind::CopyToRight) {
                copies.append({m_left + QLatin1Char('/') + action.relativePath,
                               m_right + QLatin1Char('/') + action.relativePath});
            } else if (action.kind == ActionKind::TrashRight) {
                toTrash.append(m_right + QLatin1Char('/') + action.relativePath);
            }
        }
        ops::Engine engine(journal);
        const ops::Outcome outcome = engine.synchronize(copies, toTrash, QStringLiteral("Sincronizar"), {}, m_noCancel);
        if (!outcome.errors.isEmpty() && outcome.errors.first().code == core::ErrorCode::NotSupported) {
            QSKIP("El volumen de la carpeta temporal no admite papelera en este equipo.");
        }
        QVERIFY2(outcome.ok(), qPrintable(outcome.errors.isEmpty() ? QString() : outcome.errors.first().message));
        QCOMPARE(readFile(m_right + QStringLiteral("/cambiado.txt")), QByteArray("nuevo texto"));
        QCOMPARE(readFile(m_right + QStringLiteral("/viejo.txt")), QByteArray("aaa"));
        QCOMPARE(readFile(m_right + QStringLiteral("/carpeta nueva/sub/b.txt")), QByteArray("678"));
        QCOMPARE(readFile(m_right + QStringLiteral("/común/dentro.txt")), QByteArray("uno"));
        QVERIFY(!QFile::exists(m_right + QStringLiteral("/solo derecha.txt")));
        QVERIFY(QFileInfo(m_right + QStringLiteral("/choque")).isDir());   // el choque no se tocó
        // Ahora solo queda el choque.
        const Comparison after = compare(m_left, m_right, {}, m_noCancel);
        for (const Item &item : after.items) {
            QVERIFY2(item.state == State::Identical || item.relativePath == QStringLiteral("choque"),
                     qPrintable(item.relativePath));
        }
        QCOMPARE(after.count(State::TypeConflict), 1);

        // Deshacer devuelve la derecha a como estaba.
        QVERIFY(engine.undo(outcome.operationId, m_noCancel).ok());
        QCOMPARE(readFile(m_right + QStringLiteral("/cambiado.txt")), QByteArray("viejo"));
        QCOMPARE(readFile(m_right + QStringLiteral("/viejo.txt")), QByteArray("bbbb"));
        QCOMPARE(readFile(m_right + QStringLiteral("/solo derecha.txt")), QByteArray("R"));
        QVERIFY(!QFile::exists(m_right + QStringLiteral("/carpeta nueva")));
        QCOMPARE(readFile(m_right + QStringLiteral("/común/dentro.txt")), QByteArray("dos"));
    }
};

QTEST_GUILESS_MAIN(TestSync)
#include "test_sync.moc"
