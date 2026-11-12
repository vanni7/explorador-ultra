// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/core/Log.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <thread>
#include <vector>

using namespace eu::core;

class TestLog : public QObject
{
    Q_OBJECT

private:
    static QByteArray readAll(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

private slots:
    void writesMessagesToFile()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        LogOptions options;
        options.directory = directory.path();
        options.alsoToStderr = false;
        QVERIFY(startFileLogging(options));

        qInfo("hola %d", 42);
        qWarning("cuidado: ñandú");
        const QString path = currentLogFile();
        stopFileLogging();

        const QByteArray content = readAll(path);
        QVERIFY(content.contains("INFO "));
        QVERIFY(content.contains("hola 42"));
        QVERIFY(content.contains("AVISO"));
        QVERIFY(content.contains(QByteArrayLiteral("ñandú")));
        QVERIFY(currentLogFile().isEmpty());
    }

    void rotatesAndKeepsLimitedFiles()
    {
        QTemporaryDir directory;
        LogOptions options;
        options.directory = directory.path();
        options.maxFileBytes = 2000;
        options.maxFiles = 3;
        options.alsoToStderr = false;
        QVERIFY(startFileLogging(options));
        for (int i = 0; i < 400; ++i) {
            qInfo("linea de relleno numero %d para forzar la rotacion", i);
        }
        stopFileLogging();

        const QStringList files = QDir(directory.path()).entryList(QDir::Files, QDir::Name);
        QCOMPARE(files.size(), 3);
        for (const QString &name : files) {
            QVERIFY(QFileInfo(directory.filePath(name)).size() <= 2000);
        }
        // La línea más reciente está en el archivo activo.
        QVERIFY(readAll(directory.filePath(QStringLiteral("explorador-ultra.log"))).contains("numero 399"));
    }

    void linesFromSeveralThreadsDoNotMix()
    {
        QTemporaryDir directory;
        LogOptions options;
        options.directory = directory.path();
        options.alsoToStderr = false;
        QVERIFY(startFileLogging(options));

        std::vector<std::thread> threads;
        for (int thread = 0; thread < 8; ++thread) {
            threads.emplace_back([thread] {
                for (int i = 0; i < 200; ++i) {
                    qInfo("hilo=%d mensaje=%d fin", thread, i);
                }
            });
        }
        for (std::thread &thread : threads) {
            thread.join();
        }
        const QString path = currentLogFile();
        stopFileLogging();

        const QList<QByteArray> lines = readAll(path).split('\n');
        int complete = 0;
        for (const QByteArray &line : lines) {
            if (line.isEmpty()) {
                continue;
            }
            QVERIFY2(line.endsWith(" fin"), line.constData());
            ++complete;
        }
        QCOMPARE(complete, 8 * 200);
    }

    void failsGracefullyWhenDirectoryIsNotWritable()
    {
        LogOptions options;
        // Una ruta bajo un archivo normal no se puede crear en ningún sistema.
        QTemporaryDir directory;
        QFile blocker(directory.filePath(QStringLiteral("archivo")));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        options.directory = blocker.fileName() + QStringLiteral("/sub");
        options.alsoToStderr = false;
        QVERIFY(!startFileLogging(options));
        stopFileLogging();
    }
};

QTEST_APPLESS_MAIN(TestLog)
#include "test_log.moc"
