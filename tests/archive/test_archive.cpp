// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/archive/Archive.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#ifdef EU_HAVE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#endif

using namespace eu;
using namespace eu::archive;

namespace {

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).path());
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(data);
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QStringList pathsOf(const QList<ArchiveEntry> &entries)
{
    QStringList paths;
    for (const ArchiveEntry &entry : entries) {
        paths.append(entry.path + (entry.isDirectory ? QStringLiteral("/") : QString()));
    }
    paths.sort();
    return paths;
}

/// Todo lo que hay bajo una carpeta (para comprobar que nada se escribió fuera).
QStringList everythingUnder(const QString &root)
{
    QStringList found;
    QDirIterator it(root, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        found.append(it.next().mid(root.size() + 1));
    }
    found.sort();
    return found;
}

#ifdef EU_HAVE_LIBARCHIVE
/// Una entrada para fabricar comprimidos de prueba (incluidos los maliciosos).
struct Raw
{
    QByteArray path;
    QByteArray data;
    decltype(archive_entry_filetype(nullptr)) type = AE_IFREG;   // Windows no tiene mode_t
    QByteArray link;      ///< Destino de un enlace simbólico.
    QByteArray hardlink;  ///< Destino de un enlace duro.
    qint64 zeros = 0;     ///< Escribe tantos ceros (para bombas) en vez de `data`.
};

bool writeRaw(const QString &path, const QList<Raw> &entries, bool zip = false, const char *zipOptions = nullptr)
{
    struct archive *out = archive_write_new();
    if (zip) {
        archive_write_set_format_zip(out);
        if (zipOptions != nullptr && archive_write_set_options(out, zipOptions) != ARCHIVE_OK) {
            archive_write_free(out);
            return false;
        }
    } else {
        archive_write_set_format_pax_restricted(out);
        archive_write_add_filter_gzip(out);
    }
    bool ok = archive_write_open_filename(out, QFile::encodeName(path).constData()) == ARCHIVE_OK;
    for (const Raw &raw : entries) {
        struct archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, raw.path.constData());
        archive_entry_set_filetype(entry, raw.type);
        archive_entry_set_perm(entry, raw.type == AE_IFDIR ? 0755 : 0644);
        archive_entry_set_mtime(entry, 1700000000, 0);
        if (!raw.link.isEmpty()) {
            archive_entry_set_symlink(entry, raw.link.constData());
        }
        if (!raw.hardlink.isEmpty()) {
            archive_entry_set_hardlink(entry, raw.hardlink.constData());
        }
        const qint64 size = raw.type == AE_IFREG && raw.hardlink.isEmpty() ? (raw.zeros > 0 ? raw.zeros : raw.data.size()) : 0;
        archive_entry_set_size(entry, size);
        ok = ok && archive_write_header(out, entry) == ARCHIVE_OK;
        if (ok && raw.zeros > 0) {
            const QByteArray block(1024 * 1024, '\0');
            for (qint64 done = 0; done < raw.zeros; done += block.size()) {
                archive_write_data(out, block.constData(), size_t(std::min<qint64>(block.size(), raw.zeros - done)));
            }
        } else if (ok && size > 0) {
            archive_write_data(out, raw.data.constData(), size_t(raw.data.size()));
        }
        archive_entry_free(entry);
    }
    ok = archive_write_close(out) == ARCHIVE_OK && ok;
    archive_write_free(out);
    return ok;
}
#endif

} // namespace

class TestArchive : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString path(const QString &name) const { return QDir::cleanPath(m_dir.filePath(name)); }

private slots:
    void initTestCase()
    {
        if (!isAvailable()) {
            QSKIP("Compilado sin libarchive.");
        }
        // Un árbol con nombres no ASCII, una carpeta vacía y un enlace.
        writeFile(path(QStringLiteral("origen/Fotos/ñandú fotografía.txt")), "hola ñandú");
        writeFile(path(QStringLiteral("origen/Fotos/2025/playa.jpg")), QByteArray(70000, 'j'));
        writeFile(path(QStringLiteral("origen/LEEME.md")), "# Título");
        QVERIFY(QDir().mkpath(path(QStringLiteral("origen/vacía"))));
#ifndef Q_OS_WIN
        QVERIFY(QFile::link(QStringLiteral("LEEME.md"), path(QStringLiteral("origen/enlace-al-leeme"))));
#endif
    }

    void namesAndSafePaths()
    {
        QVERIFY(isArchiveName(QStringLiteral("copia.ZIP")));
        QVERIFY(isArchiveName(QStringLiteral("x.tar.gz")));
        QVERIFY(isArchiveName(QStringLiteral("disco.iso")));
        QVERIFY(isArchiveName(QStringLiteral("libro.rar")));
        QVERIFY(!isArchiveName(QStringLiteral("foto.jpg")));
        QVERIFY(!isArchiveName(QStringLiteral(".zip")));
        QCOMPARE(safeRelativePath(QStringLiteral("a/b/c.txt")), QStringLiteral("a/b/c.txt"));
        QCOMPARE(safeRelativePath(QStringLiteral("./a//b/./c")), QStringLiteral("a/b/c"));
        QCOMPARE(safeRelativePath(QStringLiteral("a\\b.txt")), QStringLiteral("a/b.txt"));
        QVERIFY(safeRelativePath(QStringLiteral("../x")).isEmpty());
        QVERIFY(safeRelativePath(QStringLiteral("a/../../x")).isEmpty());
        QVERIFY(safeRelativePath(QStringLiteral("a/../b")).isEmpty());   // nunca se sube, aunque quede dentro
        QVERIFY(safeRelativePath(QStringLiteral("/etc/passwd")).isEmpty());
        QVERIFY(safeRelativePath(QStringLiteral("C:/Windows/x")).isEmpty());
        QVERIFY(safeRelativePath(QStringLiteral("..\\..\\x")).isEmpty());
    }

    void createListAndExtractEveryFormat_data()
    {
        QTest::addColumn<int>("format");
        QTest::newRow("zip") << int(Format::Zip);
        QTest::newRow("7z") << int(Format::SevenZip);
        QTest::newRow("tar") << int(Format::Tar);
        QTest::newRow("tar.gz") << int(Format::TarGz);
    }

    void createListAndExtractEveryFormat()
    {
        QFETCH(int, format);
        const Format kind = Format(format);
        const QString output = path(QStringLiteral("salida") + QString::number(format) + extensionOf(kind));
        QVERIFY(createArchive(output, kind, {path(QStringLiteral("origen"))}, {}).ok());
        QVERIFY(QFileInfo::exists(output));
        QVERIFY(!QFileInfo::exists(QFileInfo(output).path() + QStringLiteral("/.") + QFileInfo(output).fileName()
                                   + QStringLiteral(".parcial")));

        const auto listed = listArchive(output, {});
        QVERIFY2(listed.ok(), listed.ok() ? "" : qPrintable(listed.error().message));
        const QStringList paths = pathsOf(listed.value());
        QVERIFY2(paths.contains(QStringLiteral("origen/Fotos/ñandú fotografía.txt")), qPrintable(paths.join(QLatin1Char('|'))));
        QVERIFY(paths.contains(QStringLiteral("origen/Fotos/2025/playa.jpg")));
        QVERIFY(paths.contains(QStringLiteral("origen/vacía/")));
        QVERIFY(paths.contains(QStringLiteral("origen/Fotos/")));

        // Extraer todo en una carpeta nueva: los mismos contenidos.
        const QString destination = path(QStringLiteral("extraido") + QString::number(format));
        QVERIFY(QDir().mkpath(destination));
        const auto extracted = extractArchive(output, destination, {}, {});
        QVERIFY2(extracted.ok(), extracted.ok() ? "" : qPrintable(extracted.error().message));
        QVERIFY2(extracted.value().skipped.isEmpty(), qPrintable(extracted.value().skipped.join(QLatin1Char('\n'))));
        QCOMPARE(readFile(destination + QStringLiteral("/origen/Fotos/ñandú fotografía.txt")), QByteArray("hola ñandú"));
        QCOMPARE(readFile(destination + QStringLiteral("/origen/Fotos/2025/playa.jpg")), QByteArray(70000, 'j'));
        QVERIFY(QFileInfo(destination + QStringLiteral("/origen/vacía")).isDir());
#ifndef Q_OS_WIN
        // El enlace se guarda y se extrae como enlace (apunta dentro).
        QVERIFY(QFileInfo(destination + QStringLiteral("/origen/enlace-al-leeme")).isSymLink());
#endif
        // Extraer solo una parte.
        const QString partial = path(QStringLiteral("parcial") + QString::number(format));
        QVERIFY(QDir().mkpath(partial));
        ExtractOptions only;
        only.only = QStringList{QStringLiteral("origen/Fotos/2025")};
        QVERIFY(extractArchive(output, partial, only, {}).ok());
        QCOMPARE(everythingUnder(partial), (QStringList{QStringLiteral("origen"), QStringLiteral("origen/Fotos"),
                                                         QStringLiteral("origen/Fotos/2025"),
                                                         QStringLiteral("origen/Fotos/2025/playa.jpg")}));
    }

    void neverOverwrites()
    {
        const QString output = path(QStringLiteral("no-pisar.zip"));
        QVERIFY(createArchive(output, Format::Zip, {path(QStringLiteral("origen/LEEME.md"))}, {}).ok());
        // Crear encima de uno existente: error, y el existente sigue igual.
        const QByteArray before = readFile(output);
        const core::Status again = createArchive(output, Format::Zip, {path(QStringLiteral("origen"))}, {});
        QVERIFY(!again.ok());
        QCOMPARE(again.error().code, core::ErrorCode::AlreadyExists);
        QCOMPARE(readFile(output), before);
        // Extraer donde ya hay un archivo con ese nombre: se omite y se avisa.
        const QString destination = path(QStringLiteral("con-algo"));
        writeFile(destination + QStringLiteral("/LEEME.md"), "mío, no tocar");
        const auto extracted = extractArchive(output, destination, {}, {});
        QVERIFY(extracted.ok());
        QCOMPARE(extracted.value().skipped.size(), 1);
        QCOMPARE(readFile(destination + QStringLiteral("/LEEME.md")), QByteArray("mío, no tocar"));
    }

    void zipSlipAndLinksNeverEscape()
    {
#ifdef EU_HAVE_LIBARCHIVE
        const QString evil = path(QStringLiteral("malicioso.tar.gz"));
        QVERIFY(writeRaw(evil, {
            {"bueno.txt", "bien", AE_IFREG, {}, {}, 0},
            {"../fuera.txt", "mal", AE_IFREG, {}, {}, 0},
            {"a/../../fuera2.txt", "mal", AE_IFREG, {}, {}, 0},
            {"/tmp/eu-absoluto-prueba.txt", "mal", AE_IFREG, {}, {}, 0},
            {"C:/win.txt", "mal", AE_IFREG, {}, {}, 0},
            {"enlace", {}, AE_IFLNK, "../..", {}, 0},
            {"enlace/escapado.txt", "mal", AE_IFREG, {}, {}, 0},
            {"absoluto", {}, AE_IFLNK, "/etc", {}, 0},
            {"duro", {}, AE_IFREG, {}, "../../etc/passwd", 0},
            {"dentro", {}, AE_IFLNK, "bueno.txt", {}, 0},
        }));
        const QString box = path(QStringLiteral("caja/destino"));
        QVERIFY(QDir().mkpath(box));
        const auto extracted = extractArchive(evil, box, {}, {});
        QVERIFY2(extracted.ok(), extracted.ok() ? "" : qPrintable(extracted.error().message));
        // Solo lo seguro, y nada fuera de la carpeta de destino.
        // El enlace "enlace" -> "../.." no se crea; por eso "enlace/escapado.txt"
        // cae en una carpeta normal *dentro* del destino, que es seguro.
        QCOMPARE(everythingUnder(path(QStringLiteral("caja"))),
                 (QStringList{QStringLiteral("destino"), QStringLiteral("destino/bueno.txt"),
                              QStringLiteral("destino/dentro"), QStringLiteral("destino/enlace"),
                              QStringLiteral("destino/enlace/escapado.txt")}));
        QVERIFY(!QFileInfo(box + QStringLiteral("/enlace")).isSymLink());
        QVERIFY(extracted.value().skipped.last().startsWith(QStringLiteral("duro: es un enlace")));
        QVERIFY(!QFileInfo::exists(QStringLiteral("/tmp/eu-absoluto-prueba.txt")));
        QCOMPARE(extracted.value().skipped.size(), 7);
        QCOMPARE(readFile(box + QStringLiteral("/dentro")), QByteArray("bien"));   // el enlace interno sí
#endif
    }

    void bombsAreStopped()
    {
#ifdef EU_HAVE_LIBARCHIVE
        // 300 MB de ceros: comprimidos ocupan unos cientos de kB.
        const QString bomb = path(QStringLiteral("bomba.tar.gz"));
        QVERIFY(writeRaw(bomb, {{"ceros.bin", {}, AE_IFREG, {}, {}, 300LL * 1000 * 1000}}));
        QVERIFY(QFileInfo(bomb).size() < 2 * 1000 * 1000);
        const QString box = path(QStringLiteral("bomba"));
        QVERIFY(QDir().mkpath(box));
        ExtractOptions options;
        options.limits.maxTotalBytes = 50LL * 1000 * 1000;
        auto extracted = extractArchive(bomb, box, options, {});
        QVERIFY(!extracted.ok());
        QCOMPARE(extracted.error().code, core::ErrorCode::NoSpace);
        QVERIFY(!QFileInfo::exists(box + QStringLiteral("/ceros.bin")));   // nada a medias

        // Por proporción: más de 100 veces el tamaño del comprimido.
        ExtractOptions ratio;
        ratio.limits.maxRatio = 100;
        ratio.limits.ratioCheckFromBytes = 10LL * 1000 * 1000;
        extracted = extractArchive(bomb, box, ratio, {});
        QVERIFY(!extracted.ok());
        QVERIFY(extracted.error().message.contains(QStringLiteral("100 veces")));
        QVERIFY(!QFileInfo::exists(box + QStringLiteral("/ceros.bin")));

        // Demasiadas entradas.
        QList<Raw> many;
        for (int i = 0; i < 2000; ++i) {
            many.append({QByteArray("f") + QByteArray::number(i), "x", AE_IFREG, {}, {}, 0});
        }
        const QString crowded = path(QStringLiteral("muchas.tar.gz"));
        QVERIFY(writeRaw(crowded, many));
        Limits few;
        few.maxEntries = 1000;
        QVERIFY(!listArchive(crowded, {}, few).ok());
        ExtractOptions fewOptions;
        fewOptions.limits = few;
        QVERIFY(!extractArchive(crowded, box, fewOptions, {}).ok());
#endif
    }

    void damagedEncryptedAndCancelled()
    {
        // Un ZIP cortado a la mitad: error, sin colgarse.
        const QString whole = path(QStringLiteral("entero.zip"));
        QVERIFY(createArchive(whole, Format::Zip, {path(QStringLiteral("origen"))}, {}).ok());
        QByteArray data = readFile(whole);
        writeFile(path(QStringLiteral("cortado.zip")), data.left(data.size() / 2));
        const QString box = path(QStringLiteral("danado"));
        QVERIFY(QDir().mkpath(box));
        const auto cut = extractArchive(path(QStringLiteral("cortado.zip")), box, {}, {});
        QVERIFY(!cut.ok() || !cut.value().skipped.isEmpty() || cut.value().files < 3);
        writeFile(path(QStringLiteral("basura.zip")), QByteArray(1000, 'z'));
        QVERIFY(!listArchive(path(QStringLiteral("basura.zip")), {}).ok());

#ifdef EU_HAVE_LIBARCHIVE
        // Cifrado: se lista como tal y no se extrae sin contraseña.
        const QString locked = path(QStringLiteral("cifrado.zip"));
        if (writeRaw(locked, {{"secreto.txt", "clave", AE_IFREG, {}, {}, 0}}, true,
                     "zip:encryption=zipcrypt,zip:passphrase=1234")) {
            const auto listed = listArchive(locked, {});
            QVERIFY(listed.ok());
            QVERIFY(listed.value().first().encrypted);
            const auto extracted = extractArchive(locked, box, {}, {});
            QVERIFY(extracted.ok());
            QCOMPARE(extracted.value().files, 0);
            QVERIFY(extracted.value().skipped.first().contains(QStringLiteral("cifrado")));
        }
#endif
        // Cancelar al crear no deja nada (ni el temporal).
        tasks::CancelSource source;
        source.cancel();
        const QString never = path(QStringLiteral("nunca.zip"));
        QVERIFY(!createArchive(never, Format::Zip, {path(QStringLiteral("origen"))}, source.token()).ok());
        QVERIFY(!QFileInfo::exists(never));
        QVERIFY(!QFileInfo::exists(path(QStringLiteral(".nunca.zip.parcial"))));
    }

    void isoAndSingleGzipAreReadable()
    {
#ifdef EU_HAVE_LIBARCHIVE
        // ISO (solo lectura en la aplicación; aquí se fabrica con libarchive).
        const QString iso = path(QStringLiteral("disco.iso"));
        {
            struct archive *out = archive_write_new();
            archive_write_set_format_iso9660(out);
            QCOMPARE(archive_write_open_filename(out, QFile::encodeName(iso).constData()), ARCHIVE_OK);
            struct archive_entry *entry = archive_entry_new();
            archive_entry_set_pathname(entry, "DOCS/LEEME.TXT");
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            archive_entry_set_size(entry, 4);
            archive_write_header(out, entry);
            archive_write_data(out, "hola", 4);
            archive_entry_free(entry);
            archive_write_close(out);
            archive_write_free(out);
        }
        const auto listed = listArchive(iso, {});
        QVERIFY2(listed.ok(), listed.ok() ? "" : qPrintable(listed.error().message));
        QVERIFY(pathsOf(listed.value()).contains(QStringLiteral("DOCS/LEEME.TXT")));

        // Un .gz suelto (sin TAR): una sola entrada con el nombre sin ".gz".
        const QString gz = path(QStringLiteral("registro.log.gz"));
        {
            struct archive *out = archive_write_new();
            archive_write_set_format_raw(out);
            archive_write_add_filter_gzip(out);
            QCOMPARE(archive_write_open_filename(out, QFile::encodeName(gz).constData()), ARCHIVE_OK);
            struct archive_entry *entry = archive_entry_new();
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_write_header(out, entry);
            archive_write_data(out, "línea 1\n", 9);
            archive_entry_free(entry);
            archive_write_close(out);
            archive_write_free(out);
        }
        const auto single = listArchive(gz, {});
        QVERIFY2(single.ok(), single.ok() ? "" : qPrintable(single.error().message));
        QCOMPARE(pathsOf(single.value()), QStringList{QStringLiteral("registro.log")});
        const QString box = path(QStringLiteral("gz"));
        QVERIFY(QDir().mkpath(box));
        QVERIFY(extractArchive(gz, box, {}, {}).ok());
        QCOMPARE(readFile(box + QStringLiteral("/registro.log")), QByteArray("línea 1\n"));
#endif
    }
};

QTEST_GUILESS_MAIN(TestArchive)
#include "test_archive.moc"
