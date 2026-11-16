// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/thumbs/Thumbnail.h"
#include "eu/thumbs/ThumbnailCache.h"
#include "eu/thumbs/ThumbnailService.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageWriter>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

#include <atomic>

using namespace eu;
using namespace eu::thumbs;

namespace {

/// Imagen de prueba: mitad izquierda roja y mitad derecha azul, para poder
/// saber hacia dónde se giró.
QImage testImage(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    QPainter painter(&image);
    painter.fillRect(0, 0, width / 2, height, Qt::red);
    painter.fillRect(width / 2, 0, width - width / 2, height, Qt::blue);
    return image;
}

QByteArray jpegBytes(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "jpg", 90);
    return bytes;
}

/// Inserta en un JPEG una etiqueta EXIF de orientación (como hacen los
/// móviles al hacer una foto en vertical: los píxeles van tumbados).
QByteArray withExifOrientation(const QByteArray &jpeg, quint16 orientation)
{
    QByteArray tiff;
    tiff += QByteArray("MM\0*", 4);                                   // big endian
    tiff += QByteArray("\0\0\0\x08", 4);                              // primer IFD en el byte 8
    tiff += QByteArray("\0\x01", 2);                                  // una entrada
    tiff += QByteArray("\x01\x12", 2);                                // etiqueta Orientation
    tiff += QByteArray("\0\x03", 2);                                  // tipo SHORT
    tiff += QByteArray("\0\0\0\x01", 4);                              // un valor
    tiff += char(orientation >> 8);
    tiff += char(orientation & 0xff);
    tiff += QByteArray("\0\0", 2);
    tiff += QByteArray("\0\0\0\0", 4);                                // no hay más IFD
    const QByteArray payload = QByteArray("Exif\0\0", 6) + tiff;
    const int length = int(payload.size()) + 2;
    QByteArray segment("\xff\xe1", 2);
    segment += char(length >> 8);
    segment += char(length & 0xff);
    segment += payload;
    return jpeg.left(2) + segment + jpeg.mid(2);                      // justo después de SOI
}

void writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(bytes);
}

ThumbnailRequest requestFor(const QString &path, int maxSide = 128)
{
    const QFileInfo info(path);
    ThumbnailRequest request;
    request.path = path;
    request.size = info.size();
    request.modifiedMs = info.lastModified().toMSecsSinceEpoch();
    request.maxSide = maxSide;
    return request;
}

bool isReddish(QRgb color)
{
    return qRed(color) > 180 && qBlue(color) < 80;
}

bool isBluish(QRgb color)
{
    return qBlue(color) > 180 && qRed(color) < 80;
}

} // namespace

class TestThumbs : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    tasks::Scheduler m_scheduler;

    QString file(const QString &name) const { return m_dir.filePath(name); }

private slots:
    void knowsWhichFilesHaveAThumbnail()
    {
        QCOMPARE(thumbnailSourceFor(QStringLiteral("foto.JPG")), ThumbnailSource::Image);
        QCOMPARE(thumbnailSourceFor(QStringLiteral("a.jpeg")), ThumbnailSource::Image);
        QCOMPARE(thumbnailSourceFor(QStringLiteral("dibujo.svg")), ThumbnailSource::Image);
        QCOMPARE(thumbnailSourceFor(QStringLiteral("x.png")), ThumbnailSource::Image);
        QCOMPARE(thumbnailSourceFor(QStringLiteral("notas.txt")), ThumbnailSource::None);
        QCOMPARE(thumbnailSourceFor(QStringLiteral("libro.pdf")), ThumbnailSource::None);
        QCOMPARE(thumbnailSourceFor(QStringLiteral("sin-extension")), ThumbnailSource::None);
    }

    void keepsTheProportionAndNeverEnlarges()
    {
        QVERIFY(testImage(800, 400).save(file(QStringLiteral("ancha.png"))));
        auto thumbnail = renderThumbnail(requestFor(file(QStringLiteral("ancha.png")), 128), {});
        QVERIFY(thumbnail.ok());
        QCOMPARE(thumbnail.value().size(), QSize(128, 64));

        QVERIFY(testImage(40, 30).save(file(QStringLiteral("pequena.png"))));
        thumbnail = renderThumbnail(requestFor(file(QStringLiteral("pequena.png")), 128), {});
        QVERIFY(thumbnail.ok());
        QCOMPARE(thumbnail.value().size(), QSize(40, 30));   // no se agranda

        // Un JPEG grande se decodifica ya reducido y queda dentro del tamaño.
        writeBytes(file(QStringLiteral("grande.jpg")), jpegBytes(testImage(4000, 3000)));
        thumbnail = renderThumbnail(requestFor(file(QStringLiteral("grande.jpg")), 256), {});
        QVERIFY(thumbnail.ok());
        QVERIFY(thumbnail.value().width() <= 256 && thumbnail.value().height() <= 256);
        QVERIFY(thumbnail.value().width() >= 250);
    }

    void followsTheExifOrientation()
    {
        // Píxeles tumbados (ancha, rojo a la izquierda) con orientación 6: la
        // foto real es vertical, con el rojo arriba.
        writeBytes(file(QStringLiteral("vertical.jpg")),
                   withExifOrientation(jpegBytes(testImage(400, 200)), 6));
        const auto thumbnail = renderThumbnail(requestFor(file(QStringLiteral("vertical.jpg")), 128), {});
        QVERIFY(thumbnail.ok());
        const QImage image = thumbnail.value();
        QCOMPARE(image.size(), QSize(64, 128));
        QVERIFY(isReddish(image.pixel(32, 10)));
        QVERIFY(isBluish(image.pixel(32, 118)));
    }

    void rendersSvg()
    {
        writeBytes(file(QStringLiteral("dibujo.svg")),
                   "<svg xmlns='http://www.w3.org/2000/svg' width='200' height='100'>"
                   "<rect width='100' height='100' fill='red'/><rect x='100' width='100' height='100' fill='blue'/>"
                   "</svg>");
        const auto thumbnail = renderThumbnail(requestFor(file(QStringLiteral("dibujo.svg")), 64), {});
        QVERIFY2(thumbnail.ok(), thumbnail.ok() ? "" : qPrintable(thumbnail.error().message));
        QCOMPARE(thumbnail.value().size(), QSize(64, 32));
        QVERIFY(isReddish(thumbnail.value().pixel(10, 16)));
    }

    void brokenFilesFailWithoutCrashing()
    {
        writeBytes(file(QStringLiteral("rota.jpg")), QByteArray("\xff\xd8\xff\xe0" "basura", 10) + QByteArray(500, 'x'));
        auto thumbnail = renderThumbnail(requestFor(file(QStringLiteral("rota.jpg"))), {});
        QVERIFY(!thumbnail.ok());
        writeBytes(file(QStringLiteral("vacia.png")), QByteArray());
        thumbnail = renderThumbnail(requestFor(file(QStringLiteral("vacia.png"))), {});
        QVERIFY(!thumbnail.ok());
        // Un PNG que dice medir 100 000 × 100 000 se rechaza sin intentar decodificarlo.
        const QByteArray huge = QByteArray::fromHex("89504e470d0a1a0a0000000d49484452000186a0000186a00806000000a8520bc80000000b49444154789c63604005000010000139bd8f650000000049454e44ae426082");
        writeBytes(file(QStringLiteral("enorme.png")), huge);
        thumbnail = renderThumbnail(requestFor(file(QStringLiteral("enorme.png"))), {});
        QVERIFY(!thumbnail.ok());
        QCOMPARE(thumbnail.error().code, core::ErrorCode::NotSupported);   // el tope, no un fallo al leer
        // Una extensión equivocada no impide leerla.
        QVERIFY(testImage(50, 50).save(file(QStringLiteral("disfrazada.jpg")), "png"));
        QVERIFY(renderThumbnail(requestFor(file(QStringLiteral("disfrazada.jpg"))), {}).ok());
    }

    void cacheKeepsThumbnailsInMemoryAndOnDisk()
    {
        QTemporaryDir cacheDir;
        QVERIFY(testImage(300, 200).save(file(QStringLiteral("cache.png"))));
        const ThumbnailRequest request = requestFor(file(QStringLiteral("cache.png")));
        QImage image = renderThumbnail(request, {}).value();
        {
            ThumbnailCache cache(cacheDir.path());
            QCOMPARE(cache.lookupMemory(request).state, ThumbnailCache::State::Missing);
            cache.store(request, image);
            QCOMPARE(cache.lookupMemory(request).state, ThumbnailCache::State::Ready);
            QVERIFY(cache.memoryUsed() > 0);
        }
        // Otra caché sobre la misma carpeta (otra sesión) la encuentra en disco.
        ThumbnailCache cache(cacheDir.path());
        QCOMPARE(cache.lookupMemory(request).state, ThumbnailCache::State::Missing);
        const ThumbnailCache::Entry loaded = cache.load(request);
        QCOMPARE(loaded.state, ThumbnailCache::State::Ready);
        QCOMPARE(loaded.image.size(), image.size());
        QCOMPARE(cache.lookupMemory(request).state, ThumbnailCache::State::Ready);

        // Si el archivo cambia (otra fecha u otro tamaño), la vieja no vale.
        ThumbnailRequest changed = request;
        changed.modifiedMs += 1000;
        QCOMPARE(cache.load(changed).state, ThumbnailCache::State::Missing);
        changed = request;
        changed.maxSide = 256;
        QCOMPARE(cache.load(changed).state, ThumbnailCache::State::Missing);

        // Los fallos también se recuerdan.
        ThumbnailRequest broken = requestFor(file(QStringLiteral("rota.jpg")));
        cache.storeFailure(broken);
        ThumbnailCache another(cacheDir.path());
        QCOMPARE(another.load(broken).state, ThumbnailCache::State::Failed);

        // Recortar el disco borra lo que sobra.
        QVERIFY(cache.pruneDisk(0) >= 1);
        ThumbnailCache empty(cacheDir.path());
        QCOMPARE(empty.load(request).state, ThumbnailCache::State::Missing);
    }

    void memoryHasABudget()
    {
        ThumbnailCache cache(QString(), 1024 * 1024);   // 1 MB, solo memoria
        const QImage image = testImage(256, 256);        // 256 kB cada una
        for (int i = 0; i < 20; ++i) {
            ThumbnailRequest request;
            request.path = QStringLiteral("/x/%1.png").arg(i);
            cache.store(request, image);
        }
        QVERIFY(cache.memoryUsed() <= 1024 * 1024);
        ThumbnailRequest last;
        last.path = QStringLiteral("/x/19.png");
        QCOMPARE(cache.lookupMemory(last).state, ThumbnailCache::State::Ready);
        ThumbnailRequest first;
        first.path = QStringLiteral("/x/0.png");
        QCOMPARE(cache.lookupMemory(first).state, ThumbnailCache::State::Missing);
    }

    void serviceGeneratesOnceAndNotifies()
    {
        QTemporaryDir cacheDir;
        ThumbnailCache cache(cacheDir.path());
        ThumbnailService service(m_scheduler, cache);
        QSignalSpy ready(&service, &ThumbnailService::ready);

        QVERIFY(testImage(500, 500).save(file(QStringLiteral("servicio.png"))));
        const ThumbnailRequest request = requestFor(file(QStringLiteral("servicio.png")));
        QCOMPARE(service.request(request).state, ThumbnailCache::State::Missing);
        QCOMPARE(service.request(request).state, ThumbnailCache::State::Missing);   // no se pide dos veces
        QCOMPARE(service.pendingCount(), 1);
        QVERIFY(ready.wait(10000));
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.first().first().toString(), request.path);
        const ThumbnailCache::Entry entry = service.request(request);
        QCOMPARE(entry.state, ThumbnailCache::State::Ready);
        QCOMPARE(entry.image.size(), QSize(128, 128));

        // Un tipo sin miniatura no pide nada.
        ThumbnailRequest text;
        text.path = file(QStringLiteral("notas.txt"));
        QCOMPARE(service.request(text).state, ThumbnailCache::State::Failed);
        QCOMPARE(service.pendingCount(), 0);

        // Un archivo roto avisa igual, y queda como fallido.
        ready.clear();
        const ThumbnailRequest broken = requestFor(file(QStringLiteral("rota.jpg")));
        service.request(broken);
        QVERIFY(ready.wait(10000));
        QCOMPARE(service.request(broken).state, ThumbnailCache::State::Failed);
    }

    void obsoleteRequestsAreCancelled()
    {
        // Un solo hilo de miniaturas ocupado: lo demás queda en cola.
        tasks::SchedulerConfig config;
        config.threads[tasks::Category::Thumbnails] = 1;
        tasks::Scheduler scheduler(config);
        std::atomic<bool> release{false};
        tasks::TaskHandle blocker = scheduler.submit(tasks::Category::Thumbnails, tasks::Priority::UserInteraction,
                                                     [&](const tasks::CancelToken &) {
            while (!release) {
                QThread::msleep(1);
            }
        });
        ThumbnailCache cache{QString()};
        ThumbnailService service(scheduler, cache);
        QSignalSpy ready(&service, &ThumbnailService::ready);
        QStringList paths;
        for (int i = 0; i < 5; ++i) {
            const QString path = file(QStringLiteral("cola%1.png").arg(i));
            QVERIFY(testImage(64, 64).save(path));
            paths.append(path);
            service.request(requestFor(path));
        }
        QCOMPARE(service.pendingCount(), 5);
        // Al desplazarse solo queda a la vista la última.
        service.retainOnly({paths.last()});
        QCOMPARE(service.pendingCount(), 1);
        release = true;
        QVERIFY(blocker.waitFor(std::chrono::seconds(5)));
        QVERIFY(ready.wait(10000));
        QTest::qWait(200);
        QCOMPARE(ready.count(), 1);
        QCOMPARE(ready.first().first().toString(), paths.last());
        QCOMPARE(cache.lookupMemory(requestFor(paths.first())).state, ThumbnailCache::State::Missing);
    }
};

QTEST_GUILESS_MAIN(TestThumbs)
#include "test_thumbs.moc"
