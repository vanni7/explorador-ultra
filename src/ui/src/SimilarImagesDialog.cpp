// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/SimilarImagesDialog.h"

#include "eu/core/SizeFormat.h"
#include "eu/thumbs/Isolated.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSlider>
#include <QSplitter>
#include <QVBoxLayout>

namespace eu::ui {

namespace {

constexpr int kCardSide = 220;

/// La imagen pequeña para la huella, decodificada en el proceso auxiliar.
QImage decodeIsolated(const QString &path, const tasks::CancelToken &cancel)
{
    const QFileInfo info(path);
    thumbs::ThumbnailRequest request;
    request.path = path;
    request.size = info.size();
    request.modifiedMs = info.lastModified().toMSecsSinceEpoch();
    request.maxSide = 64;
    const core::Result<QImage> image = thumbs::renderThumbnailIsolated(request, cancel);
    return image.ok() ? image.value() : QImage();
}

} // namespace

SimilarImagesDialog::SimilarImagesDialog(tasks::Scheduler &scheduler, QString folder, QString volumeOfFolder,
                                         QStringList allVolumes, QWidget *parent)
    : QDialog(parent), m_scheduler(scheduler), m_folder(std::move(folder)), m_volume(std::move(volumeOfFolder)),
      m_allVolumes(std::move(allVolumes))
{
    setWindowTitle(tr("Imágenes parecidas"));
    resize(1100, 700);

    m_scope = new QComboBox(this);
    m_scope->addItem(tr("«%1» y sus subcarpetas").arg(QFileInfo(m_folder).fileName().isEmpty()
                                                         ? QDir::toNativeSeparators(m_folder)
                                                         : QFileInfo(m_folder).fileName()));
    if (!m_volume.isEmpty()) {
        m_scope->addItem(tr("Toda la unidad %1").arg(QDir::toNativeSeparators(m_volume)));
    }
    if (!m_allVolumes.isEmpty()) {
        m_scope->addItem(tr("Todas las unidades (%1)").arg(m_allVolumes.size()));
    }
    m_threshold = new QSlider(Qt::Horizontal, this);
    m_threshold->setRange(60, 100);
    m_threshold->setValue(90);
    m_threshold->setToolTip(tr("Más alto: solo casi idénticas (recomprimidas, reducidas). Más bajo: también retocadas, "
                               "con más riesgo de juntar fotos distintas."));
    m_thresholdLabel = new QLabel(this);
    m_searchButton = new QPushButton(tr("Buscar"), this);
    m_searchButton->setDefault(true);
    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Dónde:"), this));
    top->addWidget(m_scope, 1);
    top->addWidget(new QLabel(tr("Parecido mínimo:"), this));
    top->addWidget(m_threshold);
    top->addWidget(m_thresholdLabel);
    top->addWidget(m_searchButton);

    m_progress = new QLabel(tr("Pulsa «Buscar». Nada se quita si no lo marcas tú y lo confirmas."), this);
    m_groupList = new QListWidget(this);
    m_groupList->setAccessibleName(tr("Grupos de imágenes parecidas"));
    m_cards = new QWidget(this);
    m_cardsLayout = new QHBoxLayout(m_cards);
    m_cardsLayout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto *scroll = new QScrollArea(this);
    scroll->setWidget(m_cards);
    scroll->setWidgetResizable(true);
    auto *splitter = new QSplitter(this);
    splitter->addWidget(m_groupList);
    splitter->addWidget(scroll);
    splitter->setStretchFactor(1, 3);

    m_summary = new QLabel(this);
    m_trashButton = new QPushButton(tr("Enviar las marcadas a la papelera…"), this);
    m_trashButton->setEnabled(false);
    auto *closeButton = new QPushButton(tr("Cerrar"), this);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_summary, 1);
    bottom->addWidget(m_trashButton);
    bottom->addWidget(closeButton);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(m_progress);
    layout->addWidget(splitter, 1);
    layout->addLayout(bottom);

    const auto showThreshold = [this] { m_thresholdLabel->setText(tr("%1 %").arg(m_threshold->value())); };
    showThreshold();
    connect(m_threshold, &QSlider::valueChanged, this, showThreshold);
    connect(m_searchButton, &QPushButton::clicked, this, [this] {
        if (m_searching) {
            m_task.cancel();
        } else {
            startSearch();
        }
    });
    connect(m_groupList, &QListWidget::currentRowChanged, this, &SimilarImagesDialog::showGroup);
    connect(m_trashButton, &QPushButton::clicked, this, &SimilarImagesDialog::sendToTrash);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
    updateSummary();
}

SimilarImagesDialog::~SimilarImagesDialog()
{
    m_task.cancel();
    m_cardsTask.cancel();
}

void SimilarImagesDialog::startSearch()
{
    similar::Options options;
    switch (m_scope->currentIndex()) {
    case 0:
        options.roots = {m_folder};
        break;
    case 1:
        options.roots = m_volume.isEmpty() ? m_allVolumes : QStringList{m_volume};
        break;
    default:
        options.roots = m_allVolumes;
        break;
    }
    options.maxDistance = similar::distanceForPercent(m_threshold->value());
    m_searching = true;
    m_searchButton->setText(tr("Detener"));
    m_groupList->clear();
    m_groups.clear();
    m_marked.clear();
    showGroup(-1);
    updateSummary();
    const QPointer<SimilarImagesDialog> self(this);
    const quint64 generation = ++m_generation;
    m_task = m_scheduler.submitForPaths(
        tasks::Category::Thumbnails, tasks::Priority::UserInteraction, options.roots,
        [self, generation, options](const tasks::CancelToken &cancel) {
            const similar::Result result = similar::findSimilar(options, &decodeIsolated, cancel,
                                                                [self](const similar::Progress &progress) {
                QMetaObject::invokeMethod(qApp, [self, progress] {
                    if (self) {
                        const QLocale locale;
                        self->m_progress->setText(progress.comparing
                                                      ? tr("Comparando %1 imágenes…").arg(locale.toString(progress.found))
                                                      : tr("Leyendo imágenes: %1 de %2")
                                                            .arg(locale.toString(progress.done), locale.toString(progress.found)));
                    }
                }, Qt::QueuedConnection);
            });
            const bool stopped = cancel.isCancelled();
            QMetaObject::invokeMethod(qApp, [self, generation, result, stopped] {
                if (self) {
                    if (stopped) {
                        self->m_progress->setText(tr("Búsqueda detenida."));
                    }
                    self->showResult(generation, result);
                }
            }, Qt::QueuedConnection);
        });
}

void SimilarImagesDialog::showResult(quint64 generation, const similar::Result &result)
{
    if (generation != m_generation) {
        return;
    }
    m_searching = false;
    m_searchButton->setText(tr("Buscar"));
    m_groups = result.groups;
    for (const similar::Group &group : std::as_const(m_groups)) {
        m_groupList->addItem(tr("%1 imágenes · %2 % de parecido o más · %3")
                                 .arg(group.images.size())
                                 .arg(similar::similarityPercent(group.maxDistance))
                                 .arg(QFileInfo(group.images.first().path).fileName()));
    }
    if (m_progress->text() != tr("Búsqueda detenida.")) {
        const QLocale locale;
        QString text = m_groups.isEmpty()
            ? tr("No hay imágenes parecidas entre %1.").arg(locale.toString(result.images))
            : tr("%1 imágenes revisadas: %2 grupos de parecidas.")
                  .arg(locale.toString(result.images), locale.toString(m_groups.size()));
        if (result.unreadable > 0) {
            text += QLatin1Char(' ') + tr("(%n no se pudieron leer)", nullptr, int(result.unreadable));
        }
        m_progress->setText(text);
    }
    if (!m_groups.isEmpty()) {
        m_groupList->setCurrentRow(0);
    }
    updateSummary();
    emit searchFinished();
}

void SimilarImagesDialog::showGroup(int index)
{
    m_cardsTask.cancel();
    while (QLayoutItem *item = m_cardsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_cardImages.clear();
    m_cardDetails.clear();
    m_cardChecks.clear();
    if (index < 0 || index >= m_groups.size()) {
        return;
    }
    const similar::Group &group = m_groups[index];
    const QLocale locale;
    for (const similar::Image &image : group.images) {
        auto *card = new QWidget(m_cards);
        auto *layout = new QVBoxLayout(card);
        auto *picture = new QLabel(tr("Cargando…"), card);
        picture->setFixedSize(kCardSide, kCardSide);
        picture->setAlignment(Qt::AlignCenter);
        auto *details = new QLabel(card);
        details->setWordWrap(true);
        details->setTextInteractionFlags(Qt::TextSelectableByMouse);
        details->setText(tr("<b>%1</b><br>%2<br>%3 · %4<br>Parecido: %5 %")
                             .arg(QFileInfo(image.path).fileName().toHtmlEscaped(),
                                  QDir::toNativeSeparators(QFileInfo(image.path).absolutePath()).toHtmlEscaped(),
                                  core::formatSize(image.size),
                                  locale.toString(QDateTime::fromMSecsSinceEpoch(image.modifiedMs), QLocale::ShortFormat))
                             .arg(similar::similarityPercent(image.distance)));
        details->setFixedWidth(kCardSide);
        auto *check = new QCheckBox(tr("Quitar esta"), card);
        check->setChecked(m_marked.contains(image.path));
        connect(check, &QCheckBox::toggled, this, [this, path = image.path, check](bool on) {
            if (!setMarked(path, on)) {
                const QSignalBlocker blocker(check);
                check->setChecked(false);
            }
        });
        layout->addWidget(picture);
        layout->addWidget(details);
        layout->addWidget(check);
        layout->addStretch(1);
        m_cardsLayout->addWidget(card);
        m_cardImages.insert(image.path, picture);
        m_cardDetails.insert(image.path, details);
        m_cardChecks.insert(image.path, check);
    }
    // Imagen grande y resolución, en segundo plano y en el auxiliar.
    const QPointer<SimilarImagesDialog> self(this);
    const quint64 generation = ++m_cardsGeneration;
    QStringList paths;
    for (const similar::Image &image : group.images) {
        paths.append(image.path);
    }
    m_cardsTask = m_scheduler.submit(tasks::Category::Thumbnails, tasks::Priority::RequestedPreview,
                                     [self, generation, paths](const tasks::CancelToken &cancel) {
        static const QRegularExpression dimensions(QStringLiteral("(\\d+)\\s*[×x]\\s*(\\d+)"));
        for (const QString &path : paths) {
            if (cancel.isCancelled()) {
                return;
            }
            const QFileInfo info(path);
            thumbs::ThumbnailRequest request;
            request.path = path;
            request.size = info.size();
            request.modifiedMs = info.lastModified().toMSecsSinceEpoch();
            request.maxSide = kCardSide;
            const core::Result<QImage> image = thumbs::renderThumbnailIsolated(request, cancel);
            QString resolution;
            for (const meta::Section &section : thumbs::readMetadataIsolated(path, cancel)) {
                for (const meta::Field &field : section.fields) {
                    if (resolution.isEmpty() && field.label == QLatin1String("Dimensiones")) {
                        const QRegularExpressionMatch match = dimensions.match(field.value);
                        resolution = match.hasMatch() ? match.captured(1) + QStringLiteral(" × ") + match.captured(2)
                                                      : field.value;
                    }
                }
            }
            const QImage picture = image.ok() ? image.value() : QImage();
            QMetaObject::invokeMethod(qApp, [self, generation, path, picture, resolution] {
                if (!self || generation != self->m_cardsGeneration) {
                    return;
                }
                if (QLabel *label = self->m_cardImages.value(path)) {
                    if (picture.isNull()) {
                        label->setText(tr("No se pudo mostrar"));
                    } else {
                        label->setPixmap(QPixmap::fromImage(picture));
                    }
                }
                if (QLabel *details = self->m_cardDetails.value(path); details != nullptr && !resolution.isEmpty()) {
                    details->setText(details->text() + QStringLiteral("<br>") + tr("Resolución: %1").arg(resolution));
                }
            }, Qt::QueuedConnection);
        }
    });
}

bool SimilarImagesDialog::setMarked(const QString &path, bool marked)
{
    if (!marked) {
        m_marked.remove(path);
        updateSummary();
        return true;
    }
    // Nunca todas las de un grupo.
    for (const similar::Group &group : std::as_const(m_groups)) {
        bool inGroup = false;
        int markedInGroup = 0;
        for (const similar::Image &image : group.images) {
            inGroup = inGroup || image.path == path;
            markedInGroup += m_marked.contains(image.path) ? 1 : 0;
        }
        if (inGroup && markedInGroup + 1 >= group.images.size()) {
            m_progress->setText(tr("Se conserva siempre al menos una imagen de cada grupo."));
            return false;
        }
        if (inGroup) {
            break;
        }
    }
    m_marked.insert(path);
    updateSummary();
    return true;
}

void SimilarImagesDialog::updateSummary()
{
    qint64 bytes = 0;
    for (const QString &path : std::as_const(m_marked)) {
        bytes += QFileInfo(path).size();
    }
    m_summary->setText(m_marked.isEmpty()
                           ? tr("Nada marcado: marca a mano las que quieras quitar.")
                           : tr("%n marcada(s): %1", nullptr, int(m_marked.size())).arg(core::formatSize(bytes)));
    m_trashButton->setEnabled(!m_marked.isEmpty());
}

void SimilarImagesDialog::sendToTrash()
{
    if (m_marked.isEmpty()) {
        return;
    }
    qint64 bytes = 0;
    for (const QString &path : std::as_const(m_marked)) {
        bytes += QFileInfo(path).size();
    }
    const bool confirmed = m_confirm
        ? m_confirm(int(m_marked.size()), bytes)
        : QMessageBox::question(this, tr("Enviar a la papelera"),
                                tr("Se enviarán %n imagen(es) (%1) a la papelera propia. Se pueden recuperar desde "
                                   "allí o con Deshacer.", nullptr, int(m_marked.size()))
                                    .arg(core::formatSize(bytes)),
                                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel)
              == QMessageBox::Yes;
    if (confirmed) {
        accept();
    }
}

} // namespace eu::ui
