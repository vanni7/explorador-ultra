// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/similar/Similar.h"

#include <QDir>
#include <QGuiApplication>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>

using namespace eu;
using namespace eu::similar;

namespace {

/// Una "foto": degradado con formas, distinta según `seed`.
QImage scene(int seed)
{
    QImage image(800, 600, QImage::Format_RGB32);
    QPainter painter(&image);
    // La dirección del degradado cambia con cada "foto": es lo que más pesa en la huella.
    const QPointF corners[] = {{0, 0}, {800, 0}, {800, 600}, {0, 600}};
    QLinearGradient gradient(corners[seed % 4], corners[(seed + 2) % 4]);
    gradient.setColorAt(0, QColor::fromHsv((seed * 70) % 360, 200, 230));
    gradient.setColorAt(1, QColor::fromHsv((seed * 70 + 180) % 360, 150, 60));
    painter.fillRect(image.rect(), gradient);
    painter.setBrush(QColor::fromHsv((seed * 130) % 360, 255, 255));
    for (int i = 0; i < 5; ++i) {
        painter.drawEllipse(QPoint(100 + ((seed * 97 + i * 151) % 600), 80 + ((seed * 53 + i * 89) % 440)), 60, 40);
    }
    return image;
}

QImage brighter(const QImage &image)
{
    QImage out = image;
    for (int y = 0; y < out.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < out.width(); ++x) {
            const QColor color(row[x]);
            row[x] = QColor(std::min(255, color.red() + 20), std::min(255, color.green() + 20),
                            std::min(255, color.blue() + 20)).rgb();
        }
    }
    return out;
}

} // namespace

class TestSimilar : public QObject
{
    Q_OBJECT

private slots:
    void fingerprintsAndDistances()
    {
        const QImage a = scene(1);
        QCOMPARE(distance(fingerprint(a), fingerprint(a)), 0);
        QVERIFY(distance(fingerprint(a), fingerprint(a.scaled(200, 150))) <= 6);
        const int different = distance(fingerprint(a), fingerprint(scene(2)));
        QVERIFY2(different > 30, qPrintable(QString::number(different)));
        QVERIFY(!fingerprint(QImage()).valid);
        QCOMPARE(similarityPercent(0), 100);
        QCOMPARE(similarityPercent(128), 0);
        QCOMPARE(distanceForPercent(90), 12);
    }

    void groupsVersionsOfTheSamePicture()
    {
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        const QImage original = scene(3);
        QVERIFY(original.save(root + QStringLiteral("/original.png")));
        QVERIFY(original.save(root + QStringLiteral("/comprimida.jpg"), "JPG", 35));
        QDir().mkpath(root + QStringLiteral("/redes"));
        QVERIFY(original.scaled(320, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                    .save(root + QStringLiteral("/redes/pequena.jpg"), "JPG", 80));
        QVERIFY(brighter(original).save(root + QStringLiteral("/retocada.png")));
        QVERIFY(scene(7).save(root + QStringLiteral("/otra.png")));
        QVERIFY(scene(11).save(root + QStringLiteral("/otra mas.jpg")));
        {
            QFile broken(root + QStringLiteral("/rota.jpg"));
            QVERIFY(broken.open(QIODevice::WriteOnly));
            broken.write("no es una imagen");
        }
        Options options;
        options.roots = {root};
        int reports = 0;
        const Result result = findSimilar(options, {}, {}, [&](const Progress &) { ++reports; });
        QCOMPARE(result.images, 7);
        QCOMPARE(result.unreadable, 1);
        QCOMPARE(result.groups.size(), 1);
        QStringList names;
        for (const Image &image : result.groups.first().images) {
            names.append(QDir(root).relativeFilePath(image.path));
        }
        names.sort();
        QCOMPARE(names, (QStringList{QStringLiteral("comprimida.jpg"), QStringLiteral("original.png"),
                                     QStringLiteral("redes/pequena.jpg"), QStringLiteral("retocada.png")}));
        // La primera es la de más bytes, a distancia 0 de sí misma.
        QCOMPARE(result.groups.first().images.first().distance, 0);
        QVERIFY(reports > 0);

        // Muy retocada (medio fotograma tapado): fuera al 90 %, dentro al 50 %.
        QImage covered = original;
        {
            QPainter painter(&covered);
            painter.fillRect(QRect(0, 0, 400, 600), Qt::black);
        }
        QVERIFY(covered.save(root + QStringLiteral("/tapada.png")));
        const int far = distance(fingerprint(original), fingerprint(covered));
        QVERIFY2(far > 12 && far <= 64, qPrintable(QString::number(far)));
        const auto groupedWith = [&](const Result &found, const QString &name) {
            for (const Group &group : found.groups) {
                for (const Image &image : group.images) {
                    if (image.path.endsWith(name)) {
                        return int(group.images.size());
                    }
                }
            }
            return 0;
        };
        QCOMPARE(groupedWith(findSimilar(options, {}, {}), QStringLiteral("/tapada.png")), 0);
        Options loose = options;
        loose.maxDistance = distanceForPercent(50);
        QVERIFY(groupedWith(findSimilar(loose, {}, {}), QStringLiteral("/tapada.png")) >= 5);
        QFile::remove(root + QStringLiteral("/tapada.png"));

        // Con un decodificador propio (como el del proceso auxiliar).
        int decoded = 0;
        options.maxDistance = 12;
        const Result custom = findSimilar(options, [&](const QString &path, const tasks::CancelToken &) {
            ++decoded;
            return decodeSmall(path);
        }, {});
        QCOMPARE(decoded, 7);
        QCOMPARE(custom.groups.size(), 1);
    }
};

int main(int argc, char *argv[])
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication application(argc, argv);
    TestSimilar test;
    QTEST_SET_MAIN_SOURCE_PATH
    return QTest::qExec(&test, argc, argv);
}

#include "test_similar.moc"
