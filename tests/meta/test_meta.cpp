// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/meta/Metadata.h"

#include "../support/TestMedia.h"

#include <QDir>
#include <QGuiApplication>
#include <QImageReader>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QTest>

#ifdef EU_HAVE_EXIV2
#include <exiv2/exiv2.hpp>
#endif

using namespace eu;
using namespace eu::meta;
using namespace eu::testsupport;

namespace {

/// El valor de un campo, o vacío si no está.
QString value(const QList<Section> &sections, const QString &section, const QString &label)
{
    for (const Section &candidate : sections) {
        if (candidate.title != section) {
            continue;
        }
        for (const Field &field : candidate.fields) {
            if (field.label == label) {
                return field.value;
            }
        }
    }
    return {};
}

bool hasSection(const QList<Section> &sections, const QString &title)
{
    for (const Section &section : sections) {
        if (section.title == title) {
            return true;
        }
    }
    return false;
}

} // namespace

class TestMeta : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString file(const QString &name) const { return m_dir.filePath(name); }

private slots:
    void imagesHaveTheirDimensions()
    {
        QImage image(300, 200, QImage::Format_RGB32);
        image.fill(Qt::green);
        QVERIFY(image.save(file(QStringLiteral("foto.png"))));
        const QList<Section> sections = readMetadata(file(QStringLiteral("foto.png")), {});
        QCOMPARE(value(sections, QStringLiteral("Imagen"), QStringLiteral("Dimensiones")),
                 QStringLiteral("300 × 200 píxeles"));
        QCOMPARE(value(sections, QStringLiteral("Imagen"), QStringLiteral("Formato")), QStringLiteral("PNG"));
        // Lo que no aplica no aparece: una PNG sin EXIF no tiene sección de cámara.
        QVERIFY(!hasSection(sections, QStringLiteral("Cámara")));
    }

    void photosHaveTheirExif()
    {
#ifndef EU_HAVE_EXIV2
        QSKIP("Compilado sin Exiv2.");
#else
        QVERIFY(hasExifSupport());
        const QString path = file(QStringLiteral("camara.jpg"));
        QImage image(320, 240, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        QVERIFY(image.save(path, "jpg"));
        {
            auto photo = Exiv2::ImageFactory::open(QFile::encodeName(path).toStdString());
            photo->readMetadata();
            Exiv2::ExifData exif;
            exif["Exif.Image.Make"] = "Canon";
            exif["Exif.Image.Model"] = "Canon EOS 80D";
            exif["Exif.Photo.DateTimeOriginal"] = "2024:03:12 16:57:40";
            exif["Exif.Photo.ExposureTime"] = Exiv2::URational(1, 125);
            exif["Exif.Photo.FNumber"] = Exiv2::URational(28, 10);
            exif["Exif.Photo.ISOSpeedRatings"] = uint16_t(200);
            exif["Exif.Photo.FocalLength"] = Exiv2::URational(35, 1);
            // 19° 25' 57" N, 99° 7' 59" W: el Zócalo de la Ciudad de México.
            Exiv2::URationalValue latitude;
            latitude.read("19/1 25/1 57/1");
            exif.add(Exiv2::ExifKey("Exif.GPSInfo.GPSLatitude"), &latitude);
            exif["Exif.GPSInfo.GPSLatitudeRef"] = "N";
            Exiv2::URationalValue longitude;
            longitude.read("99/1 7/1 59/1");
            exif.add(Exiv2::ExifKey("Exif.GPSInfo.GPSLongitude"), &longitude);
            exif["Exif.GPSInfo.GPSLongitudeRef"] = "W";
            photo->setExifData(exif);
            photo->writeMetadata();
        }
        const QList<Section> sections = readMetadata(path, {});
        QCOMPARE(value(sections, QStringLiteral("Imagen"), QStringLiteral("Dimensiones")),
                 QStringLiteral("320 × 240 píxeles"));
        // La marca no se repite si el modelo ya la lleva.
        QCOMPARE(value(sections, QStringLiteral("Cámara"), QStringLiteral("Cámara")), QStringLiteral("Canon EOS 80D"));
        QCOMPARE(value(sections, QStringLiteral("Cámara"), QStringLiteral("Fecha de la foto")),
                 QStringLiteral("2024-03-12 16:57:40"));
        QCOMPARE(value(sections, QStringLiteral("Cámara"), QStringLiteral("Exposición")), QStringLiteral("1/125 s"));
        QCOMPARE(value(sections, QStringLiteral("Cámara"), QStringLiteral("Apertura")), QStringLiteral("F2.8"));
        QCOMPARE(value(sections, QStringLiteral("Cámara"), QStringLiteral("ISO")), QStringLiteral("200"));
        QCOMPARE(value(sections, QStringLiteral("Cámara"), QStringLiteral("Distancia focal")), QStringLiteral("35.0 mm"));
        QCOMPARE(value(sections, QStringLiteral("Cámara"), QStringLiteral("Ubicación (GPS)")),
                 QStringLiteral("19.432500, -99.133056"));
#endif
    }

    void pdfHasItsPageCount()
    {
        if (!QImageReader::supportedImageFormats().contains("pdf")) {
            QSKIP("Esta instalación de Qt no tiene el complemento de PDF.");
        }
        const QString path = file(QStringLiteral("tres.pdf"));
        {
            QPdfWriter writer(path);
            QPainter painter(&writer);
            painter.drawText(100, 100, QStringLiteral("Uno"));
            writer.newPage();
            painter.drawText(100, 100, QStringLiteral("Dos"));
            writer.newPage();
            painter.drawText(100, 100, QStringLiteral("Tres"));
        }
        QCOMPARE(value(readMetadata(path, {}), QStringLiteral("Documento"), QStringLiteral("Páginas")),
                 QStringLiteral("3"));
    }

    void mediaHasDurationCodecsAndTags()
    {
#ifndef EU_HAVE_FFMPEG
        QSKIP("Compilado sin FFmpeg.");
#else
        QVERIFY(hasMediaSupport());
        const QString video = file(QStringLiteral("video.mov"));
        QVERIFY(writeColorVideo(video, {{Qt::red, 50}}, 10, QSize(160, 120)));
        QList<Section> sections = readMetadata(video, {});
        QCOMPARE(value(sections, QStringLiteral("Multimedia"), QStringLiteral("Duración")), QStringLiteral("0:05"));
        QCOMPARE(value(sections, QStringLiteral("Video"), QStringLiteral("Códec")), QStringLiteral("mjpeg"));
        QCOMPARE(value(sections, QStringLiteral("Video"), QStringLiteral("Resolución")), QStringLiteral("160 × 120"));
        QVERIFY(!hasSection(sections, QStringLiteral("Audio")));

        const QString song = file(QStringLiteral("cancion.flac"));
        QVERIFY(writeFlac(song, 2.0, QImage(), {{"title", "Blue Moon of Kentucky"}, {"artist", "Elvis Presley"},
                                                {"album", "A Date With Elvis"}}));
        sections = readMetadata(song, {});
        QCOMPARE(value(sections, QStringLiteral("Etiquetas"), QStringLiteral("Título")),
                 QStringLiteral("Blue Moon of Kentucky"));
        QCOMPARE(value(sections, QStringLiteral("Etiquetas"), QStringLiteral("Artista")), QStringLiteral("Elvis Presley"));
        QCOMPARE(value(sections, QStringLiteral("Etiquetas"), QStringLiteral("Álbum")), QStringLiteral("A Date With Elvis"));
        QCOMPARE(value(sections, QStringLiteral("Audio"), QStringLiteral("Códec")), QStringLiteral("flac"));
        QCOMPARE(value(sections, QStringLiteral("Audio"), QStringLiteral("Canales")), QStringLiteral("1 (mono)"));
        QCOMPARE(value(sections, QStringLiteral("Multimedia"), QStringLiteral("Duración")), QStringLiteral("0:02"));
        QVERIFY(!hasSection(sections, QStringLiteral("Video")));
#endif
    }

    void brokenOrMissingFilesGiveNothing()
    {
        QVERIFY(readMetadata(file(QStringLiteral("no-existe.jpg")), {}).isEmpty());
        QVERIFY(readMetadata(m_dir.path(), {}).isEmpty());   // una carpeta
        QFile broken(file(QStringLiteral("roto.mp4")));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write(QByteArray(2048, 'x'));
        broken.close();
        QVERIFY(readMetadata(broken.fileName(), {}).isEmpty());
        QFile fake(file(QStringLiteral("falsa.jpg")));
        QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write("no es una imagen");
        fake.close();
        QVERIFY(!hasSection(readMetadata(fake.fileName(), {}), QStringLiteral("Cámara")));
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication application(argc, argv);
    TestMeta test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "test_meta.moc"
