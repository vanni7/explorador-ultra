// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/trash/Trash.h"

#include "eu/fs/Volumes.h"
#include "eu/ops/FileOps.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

using namespace eu;
using namespace eu::trash;

namespace {

void writeFile(const QString &path, const QByteArray &content)
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(content);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<no se pudo leer>");
}

} // namespace

/// Estas pruebas usan la papelera real del volumen donde vive la carpeta
/// temporal, pero solo tocan los elementos que ellas mismas envían, y los
/// purgan al terminar.
class TestTrash : public QObject
{
    Q_OBJECT

private:
    QList<TrashItem> m_toPurge;

private slots:
    void cleanup()
    {
        for (const TrashItem &item : std::as_const(m_toPurge)) {
            purgeItem(item, tasks::CancelToken());
        }
        m_toPurge.clear();
    }

    void trashDirectoryIsOnTheSameVolume()
    {
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("a")), "a");
        const auto trashDirectory = trashDirectoryFor(directory.filePath(QStringLiteral("a")));
        if (!trashDirectory.ok()) {
            QSKIP("El volumen de la carpeta temporal no admite papelera en este equipo.");
        }
        QVERIFY(fs::isSameVolume(trashDirectory.value(), directory.path()));
        QVERIFY(QFileInfo(trashDirectory.value()).fileName() == trashFolderName());
        QVERIFY(isInsideTrash(trashDirectory.value() + QStringLiteral("/x/y")));
        QVERIFY(isInsideTrash(trashDirectory.value()));
        QVERIFY(!isInsideTrash(directory.path()));
    }

    void fileGoesToTrashAndComesBack()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("informe ñ.txt"));
        writeFile(path, "contenido importante");

        const auto trashed = trashPath(path);
        if (!trashed.ok() && trashed.error().code == core::ErrorCode::NotSupported) {
            QSKIP("El volumen de la carpeta temporal no admite papelera en este equipo.");
        }
        QVERIFY2(trashed.ok(), trashed.ok() ? "" : qPrintable(trashed.error().message));
        m_toPurge.append(trashed.value());
        const TrashItem item = trashed.value();

        QVERIFY(!QFileInfo::exists(path));
        QCOMPARE(readFile(item.storedPath()), QByteArray("contenido importante"));
        QVERIFY(QFileInfo::exists(item.infoPath()));
        QCOMPARE(item.originalPath, QDir::cleanPath(path));
        QCOMPARE(item.size, qint64(20));

        // Aparece en el listado de esa papelera, leído solo de los .info.
        const auto listed = listTrash(item.trashDirectory);
        QVERIFY(listed.ok());
        const bool found = std::any_of(listed.value().begin(), listed.value().end(),
                                       [&](const TrashItem &other) { return other.id == item.id; });
        QVERIFY(found);
        // La vista unificada recorre las unidades visibles para el usuario; una
        // carpeta temporal en memoria (tmpfs) no es una de ellas.
        const QList<fs::Volume> volumes = fs::listVolumes();
        const QString volumeRoot = fs::volumeForPath(item.trashDirectory).rootPath;
        const bool visibleVolume = std::any_of(volumes.begin(), volumes.end(), [&](const fs::Volume &volume) {
            return volume.rootPath == volumeRoot;
        });
        if (visibleVolume || item.trashDirectory == QDir::home().filePath(trashFolderName())) {
            QVERIFY(existingTrashDirectories().contains(item.trashDirectory));
        }

        QVERIFY(restoreItem(item).ok());
        QCOMPARE(readFile(path), QByteArray("contenido importante"));
        QVERIFY(!QFileInfo::exists(item.infoPath()));
        QVERIFY(!QFileInfo::exists(item.trashDirectory + QLatin1Char('/') + item.id));
    }

    void folderWithContentGoesAndComesBack()
    {
        QTemporaryDir directory;
        const QString folder = directory.filePath(QStringLiteral("álbum"));
        QVERIFY(QDir().mkpath(folder + QStringLiteral("/sub")));
        writeFile(folder + QStringLiteral("/sub/foto.jpg"), "jpg");

        const auto trashed = trashPath(folder);
        if (!trashed.ok() && trashed.error().code == core::ErrorCode::NotSupported) {
            QSKIP("El volumen de la carpeta temporal no admite papelera en este equipo.");
        }
        QVERIFY(trashed.ok());
        m_toPurge.append(trashed.value());
        QVERIFY(trashed.value().isDirectory);
        QVERIFY(!QFileInfo::exists(folder));

        QVERIFY(restoreItem(trashed.value()).ok());
        QCOMPARE(readFile(folder + QStringLiteral("/sub/foto.jpg")), QByteArray("jpg"));
    }

    void sameNameTwiceDoesNotCollide()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("nota.txt"));
        writeFile(path, "primera");
        const auto first = trashPath(path);
        if (!first.ok() && first.error().code == core::ErrorCode::NotSupported) {
            QSKIP("El volumen de la carpeta temporal no admite papelera en este equipo.");
        }
        QVERIFY(first.ok());
        m_toPurge.append(first.value());
        writeFile(path, "segunda");
        const auto second = trashPath(path);
        QVERIFY(second.ok());
        m_toPurge.append(second.value());

        QVERIFY(first.value().id != second.value().id);
        QCOMPARE(readFile(first.value().storedPath()), QByteArray("primera"));
        QCOMPARE(readFile(second.value().storedPath()), QByteArray("segunda"));
    }

    void restoreNeverOverwrites()
    {
        QTemporaryDir directory;
        const QString path = directory.filePath(QStringLiteral("dato.txt"));
        writeFile(path, "antiguo");
        const auto trashed = trashPath(path);
        if (!trashed.ok() && trashed.error().code == core::ErrorCode::NotSupported) {
            QSKIP("El volumen de la carpeta temporal no admite papelera en este equipo.");
        }
        QVERIFY(trashed.ok());
        m_toPurge.append(trashed.value());
        writeFile(path, "nuevo");   // alguien creó otro con el mismo nombre

        const core::Status blocked = restoreItem(trashed.value());
        QVERIFY(!blocked.ok());
        QCOMPARE(blocked.error().code, core::ErrorCode::AlreadyExists);
        QCOMPARE(readFile(path), QByteArray("nuevo"));
        QCOMPARE(readFile(trashed.value().storedPath()), QByteArray("antiguo"));   // sigue en la papelera

        // Se puede restaurar con otro nombre.
        const QString alternative = directory.filePath(QStringLiteral("dato (2).txt"));
        QVERIFY(restoreItem(trashed.value(), alternative).ok());
        QCOMPARE(readFile(alternative), QByteArray("antiguo"));
    }

    void restoreFailsCleanlyWhenOriginalFolderIsGone()
    {
        QTemporaryDir directory;
        const QString folder = directory.filePath(QStringLiteral("temporal"));
        QVERIFY(QDir().mkdir(folder));
        const QString path = folder + QStringLiteral("/x.txt");
        writeFile(path, "x");
        const auto trashed = trashPath(path);
        if (!trashed.ok() && trashed.error().code == core::ErrorCode::NotSupported) {
            QSKIP("El volumen de la carpeta temporal no admite papelera en este equipo.");
        }
        QVERIFY(trashed.ok());
        m_toPurge.append(trashed.value());
        QVERIFY(QDir().rmdir(folder));

        const core::Status status = restoreItem(trashed.value());
        QCOMPARE(status.error().code, core::ErrorCode::NotFound);
        QVERIFY(QFileInfo::exists(trashed.value().storedPath()));
    }

    void purgeDeletesForGoodWithoutFollowingLinks()
    {
        QTemporaryDir directory;
        const QString outside = directory.filePath(QStringLiteral("fuera"));
        QVERIFY(QDir().mkdir(outside));
        writeFile(outside + QStringLiteral("/tesoro"), "intacto");

        const QString folder = directory.filePath(QStringLiteral("borrar"));
        QVERIFY(QDir().mkpath(folder + QStringLiteral("/a/b")));
        writeFile(folder + QStringLiteral("/a/b/c.txt"), "c");
#ifdef Q_OS_UNIX
        QVERIFY(QFile::link(outside, folder + QStringLiteral("/enlace-a-fuera")));
#endif
        const auto trashed = trashPath(folder);
        if (!trashed.ok() && trashed.error().code == core::ErrorCode::NotSupported) {
            QSKIP("El volumen de la carpeta temporal no admite papelera en este equipo.");
        }
        QVERIFY(trashed.ok());

        QVERIFY(purgeItem(trashed.value(), tasks::CancelToken()).ok());
        QVERIFY(!QFileInfo::exists(trashed.value().storedPath()));
        QVERIFY(!QFileInfo::exists(trashed.value().infoPath()));
        // Lo que estaba detrás del enlace no se tocó.
        QCOMPARE(readFile(outside + QStringLiteral("/tesoro")), QByteArray("intacto"));
    }

    void missingPathAndTrashInsideTrashAreRejected()
    {
        QTemporaryDir directory;
        const auto missing = trashPath(directory.filePath(QStringLiteral("no-existe")));
        QCOMPARE(missing.error().code, core::ErrorCode::NotFound);

        const QString path = directory.filePath(QStringLiteral("x"));
        writeFile(path, "x");
        const auto trashed = trashPath(path);
        if (!trashed.ok() && trashed.error().code == core::ErrorCode::NotSupported) {
            QSKIP("El volumen de la carpeta temporal no admite papelera en este equipo.");
        }
        QVERIFY(trashed.ok());
        m_toPurge.append(trashed.value());
        const auto again = trashPath(trashed.value().storedPath());
        QVERIFY(!again.ok());
    }

    void tamperedInfoFilesAreIgnored()
    {
        QTemporaryDir directory;
        const QString fakeTrash = directory.filePath(trashFolderName());
        QVERIFY(QDir().mkdir(fakeTrash));
        // Un .info cuyo id no coincide con su nombre de archivo.
        writeFile(fakeTrash + QStringLiteral("/abc.info"),
                  R"({"id":"../../etc","originalPath":"/tmp/x","deletedMs":1,"size":1,"isDirectory":false})");
        writeFile(fakeTrash + QStringLiteral("/roto.info"), "esto no es JSON");
        const auto listed = listTrash(fakeTrash);
        QVERIFY(listed.ok());
        QVERIFY(listed.value().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestTrash)
#include "test_trash.moc"
