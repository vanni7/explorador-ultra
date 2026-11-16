// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/PreviewPanel.h"

#include "eu/archive/Archive.h"
#include "eu/meta/Metadata.h"
#include "eu/thumbs/Isolated.h"
#include "eu/thumbs/Thumbnail.h"
#include "eu/ui/Locations.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStringDecoder>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace eu::ui {

namespace {

/// Lado de la imagen que se genera para la vista previa. Mayor que el panel
/// habitual, para poder agrandarlo sin volver a leer el archivo.
constexpr int pictureSide = 1024;
/// Lo que se lee de un texto: suficiente para verlo, sin cargar un registro
/// de varios GB entero (SPEC §7.1: lectura parcial).
constexpr qint64 textBytes = 512 * 1024;

bool isMarkdown(const QString &suffix)
{
    return suffix == QLatin1String("md") || suffix == QLatin1String("markdown");
}

QString dateText(qint64 milliseconds)
{
    // "29 oct 2026 18:21:44": completa pero sin el día de la semana ni la zona.
    return milliseconds == 0 ? QString()
                             : QLocale().toString(QDateTime::fromMSecsSinceEpoch(milliseconds),
                                                  QStringLiteral("d MMM yyyy HH:mm:ss"));
}

} // namespace

struct PreviewPanel::Loaded
{
    QList<QPair<QString, QList<QPair<QString, QString>>>> sections;   ///< Título y campos.
    QImage picture;
    QString text;
    bool truncated = false;
    bool markdown = false;
    QString problem;
};

PreviewPanel::PreviewPanel(tasks::Scheduler &scheduler, QWidget *parent) : QWidget(parent), m_scheduler(scheduler)
{
    setAccessibleName(tr("Previsualización"));
    m_title = new QLabel(this);
    m_title->setWordWrap(true);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.15);
    m_title->setFont(titleFont);

    m_fieldsHost = new QWidget;
    m_fields = new QFormLayout(m_fieldsHost);
    m_fields->setLabelAlignment(Qt::AlignRight);
    m_fields->setRowWrapPolicy(QFormLayout::WrapLongRows);
    m_fieldsArea = new QScrollArea(this);
    m_fieldsArea->setWidget(m_fieldsHost);
    m_fieldsArea->setWidgetResizable(true);
    m_fieldsArea->setFrameShape(QFrame::NoFrame);

    m_preview = new QStackedWidget(this);
    m_pictureLabel = new QLabel(m_preview);
    m_pictureLabel->setAlignment(Qt::AlignCenter);
    m_pictureLabel->setMinimumSize(1, 1);   // que el panel se pueda estrechar
    m_pictureLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_pictureLabel->installEventFilter(this);   // al cambiar de tamaño se vuelve a escalar
    m_textView = new QPlainTextEdit(m_preview);
    m_textView->setReadOnly(true);
    m_textView->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_textView->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_markdownView = new QTextBrowser(m_preview);
    // Un Markdown puede enlazar a lo que sea: no se abre nada desde aquí.
    m_markdownView->setOpenLinks(false);
    m_markdownView->setOpenExternalLinks(false);
    m_message = new QLabel(m_preview);
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);
    m_message->setEnabled(false);
    m_preview->addWidget(m_pictureLabel);
    m_preview->addWidget(m_textView);
    m_preview->addWidget(m_markdownView);
    m_preview->addWidget(m_message);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(m_title);
    layout->addWidget(m_fieldsArea, 2);
    layout->addWidget(m_preview, 3);

    m_delay.setSingleShot(true);
    m_delay.setInterval(120);
    connect(&m_delay, &QTimer::timeout, this, &PreviewPanel::startLoading);
    clear();
}

PreviewPanel::~PreviewPanel()
{
    if (m_task.isValid()) {
        m_task.cancel();
    }
}

void PreviewPanel::setSizeUnits(core::SizeUnits units)
{
    if (m_units != units) {
        m_units = units;
        if (m_hasItem) {
            showItem(m_item);
        }
    }
}

void PreviewPanel::clear(const QString &message)
{
    ++m_generation;
    if (m_task.isValid()) {
        m_task.cancel();
        m_task = tasks::TaskHandle();
    }
    m_delay.stop();
    m_hasItem = false;
    m_loading = false;
    m_picture = QImage();
    m_title->clear();
    rebuildFields({});
    m_message->setText(message.isEmpty() ? tr("Selecciona un archivo para ver aquí sus datos y su contenido")
                                         : message);
    m_preview->setCurrentWidget(m_message);
}

void PreviewPanel::showItem(const PreviewItem &item)
{
    ++m_generation;
    if (m_task.isValid()) {
        m_task.cancel();
        m_task = tasks::TaskHandle();
    }
    m_item = item;
    m_hasItem = true;
    m_picture = QImage();
    m_title->setText(item.entry.name);
    rebuildFields({});   // lo común se ve ya; lo propio del tipo, cuando se lea
    m_message->setText(tr("Cargando…"));
    m_preview->setCurrentWidget(m_message);
    m_loading = true;
    m_delay.start();
}

void PreviewPanel::rebuildFields(const QList<QPair<QString, QList<QPair<QString, QString>>>> &extra)
{
    while (m_fields->rowCount() > 0) {
        m_fields->removeRow(0);
    }
    if (!m_hasItem) {
        return;
    }
    const auto addRow = [this](const QString &label, const QString &value) {
        if (value.isEmpty()) {
            return;
        }
        auto *valueLabel = new QLabel(value);
        valueLabel->setWordWrap(true);
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        valueLabel->setObjectName(label);   // para encontrarlo (pruebas, accesibilidad)
        m_fields->addRow(label + QLatin1Char(':'), valueLabel);
    };
    const auto addHeading = [this](const QString &text) {
        auto *heading = new QLabel(text);
        QFont font = heading->font();
        font.setBold(true);
        heading->setFont(font);
        m_fields->addRow(heading);
    };
    const fs::Entry &entry = m_item.entry;
    if (m_item.selectedCount > 1) {
        addRow(tr("Selección"), tr("%1 elementos (se muestra el actual)").arg(m_item.selectedCount));
    }
    addRow(tr("Tipo"), m_item.typeText);
    if (!entry.isDirectory) {
        addRow(tr("Tamaño"), tr("%1 (%2 bytes)").arg(core::formatSize(entry.size, m_units), QLocale().toString(entry.size)));
    }
    addRow(tr("Modificado"), dateText(entry.modifiedMs));
    addRow(tr("Creado"), dateText(entry.createdMs));
    if (isArchiveLocation(m_item.path)) {
        // Dentro de un comprimido: "vacaciones.zip › fotos".
        const QString inner = archiveInnerOf(m_item.path);
        const qsizetype slash = inner.lastIndexOf(QLatin1Char('/'));
        QString where = QDir::toNativeSeparators(archivePathOf(m_item.path));
        if (slash > 0) {
            where += QStringLiteral(" › ") + inner.left(slash);
        }
        addRow(tr("Ubicación"), where);
    } else {
        addRow(tr("Ubicación"), QDir::toNativeSeparators(QFileInfo(m_item.path).absolutePath()));
    }
    if (entry.isSymlink) {
        addRow(tr("Enlace a"), QDir::toNativeSeparators(QFileInfo(m_item.path).symLinkTarget()));
    }
    if (entry.isHidden) {
        addRow(tr("Oculto"), tr("Sí"));
    }
    for (const auto &[title, fields] : extra) {
        addHeading(title);
        for (const auto &[label, value] : fields) {
            addRow(label, value);
        }
    }
}

void PreviewPanel::startLoading()
{
    if (!m_hasItem) {
        return;
    }
    const QPointer<PreviewPanel> self(this);
    const quint64 generation = m_generation;
    const PreviewItem shownItem = m_item;
    const qreal ratio = devicePixelRatioF();
    // Lo pedido por el usuario va antes que las miniaturas del fondo.
    m_task = m_scheduler.submitForPaths(
        tasks::Category::Thumbnails, tasks::Priority::RequestedPreview,
        {isArchiveLocation(shownItem.path) ? archivePathOf(shownItem.path) : shownItem.path},
        [self, generation, item = shownItem, ratio](const tasks::CancelToken &cancel) mutable {
            Loaded loaded;
            // Algo de dentro de un comprimido: se saca ese archivo a una carpeta
            // temporal y se muestra desde allí (los muy grandes, no).
            if (isArchiveLocation(item.path)) {
                const auto finishWith = [&](const QString &text) {
                    loaded.problem = text;
                    QMetaObject::invokeMethod(qApp, [self, generation, loaded] {
                        if (self) {
                            self->onLoaded(generation, loaded);
                        }
                    }, Qt::QueuedConnection);
                };
                if (item.entry.isDirectory) {
                    finishWith(tr("Carpeta dentro de un comprimido"));
                    return;
                }
                if (item.entry.size > 200LL * 1000 * 1000) {
                    finishWith(tr("Demasiado grande para verlo sin extraerlo"));
                    return;
                }
                const core::Result<QString> extracted =
                    archive::extractEntryToTemp(archivePathOf(item.path), archiveInnerOf(item.path), cancel);
                if (!extracted.ok()) {
                    if (extracted.error().code != core::ErrorCode::Cancelled) {
                        finishWith(extracted.error().message);
                    }
                    return;
                }
                item.path = extracted.value();
            }
            const QString suffix = QFileInfo(item.entry.name).suffix().toLower();
            if (!item.entry.isDirectory) {
                // En el proceso auxiliar: un EXIF o unas etiquetas hechos para
                // romper el lector no cierran la aplicación.
                const QList<meta::Section> sections = thumbs::readMetadataIsolated(item.path, cancel);
                for (const meta::Section &section : sections) {
                    QList<QPair<QString, QString>> fields;
                    for (const meta::Field &field : section.fields) {
                        fields.append({field.label, field.value});
                    }
                    loaded.sections.append({section.title, fields});
                }
            }
            thumbs::ThumbnailRequest request;
            request.path = item.path;
            request.size = item.entry.size;
            request.modifiedMs = item.entry.modifiedMs;
            request.isDirectory = item.entry.isDirectory;
            request.maxSide = int(pictureSide * std::min<qreal>(ratio, 2.0));
            const thumbs::ThumbnailSource source = thumbs::thumbnailSourceFor(request);
            if (source == thumbs::ThumbnailSource::Text) {
                QFile file(item.path);
                if (file.open(QIODevice::ReadOnly)) {
                    const QByteArray data = file.read(textBytes);
                    loaded.truncated = file.size() > textBytes;
                    QStringDecoder utf8(QStringDecoder::Utf8);
                    loaded.text = utf8.decode(data);
                    if (utf8.hasError()) {
                        loaded.text = QString::fromLatin1(data);
                    }
                    loaded.markdown = isMarkdown(suffix);
                } else {
                    loaded.problem = file.errorString();
                }
            } else if (source != thumbs::ThumbnailSource::None) {
                const core::Result<QImage> picture = thumbs::renderThumbnailIsolated(request, cancel);
                if (picture.ok()) {
                    loaded.picture = picture.value();
                } else if (picture.error().code != core::ErrorCode::Cancelled) {
                    loaded.problem = picture.error().message;
                }
            }
            QMetaObject::invokeMethod(qApp, [self, generation, loaded] {
                if (self) {
                    self->onLoaded(generation, loaded);
                }
            }, Qt::QueuedConnection);
        });
}

void PreviewPanel::onLoaded(quint64 generation, const Loaded &loaded)
{
    if (generation != m_generation) {
        return;   // la selección cambió mientras se leía
    }
    m_loading = false;
    m_task = tasks::TaskHandle();
    rebuildFields(loaded.sections);
    if (!loaded.picture.isNull()) {
        m_picture = loaded.picture;
        m_picture.setDevicePixelRatio(1.0);
        // Primero a la vista, para que la etiqueta tenga ya su tamaño real.
        m_preview->setCurrentWidget(m_pictureLabel);
        updatePicture();
    } else if (loaded.markdown) {
        m_markdownView->setMarkdown(loaded.text);
        m_preview->setCurrentWidget(m_markdownView);
    } else if (!loaded.text.isNull()) {
        QString text = loaded.text;
        if (loaded.truncated) {
            text += QLatin1Char('\n') + tr("[… solo se muestra el principio del archivo]");
        }
        m_textView->setPlainText(text);
        m_preview->setCurrentWidget(m_textView);
    } else {
        m_message->setText(loaded.problem.isEmpty() ? tr("No hay vista previa para este tipo de archivo")
                                                    : tr("No se puede mostrar: %1").arg(loaded.problem));
        m_preview->setCurrentWidget(m_message);
    }
}

void PreviewPanel::updatePicture()
{
    if (m_picture.isNull()) {
        return;
    }
    const qreal ratio = devicePixelRatioF();
    QSize available = m_pictureLabel->size() * ratio;
    QSize shown = m_picture.size();
    if (shown.width() > available.width() || shown.height() > available.height()) {
        shown.scale(available, Qt::KeepAspectRatio);
    }
    QPixmap pixmap = QPixmap::fromImage(m_picture.scaled(shown, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    pixmap.setDevicePixelRatio(ratio);
    m_pictureLabel->setPixmap(pixmap);
}

void PreviewPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updatePicture();
}

bool PreviewPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_pictureLabel && event->type() == QEvent::Resize) {
        updatePicture();
    }
    return QWidget::eventFilter(watched, event);
}

QString PreviewPanel::title() const
{
    return m_title->text();
}

QString PreviewPanel::fieldValue(const QString &label) const
{
    const QLabel *value = m_fieldsHost->findChild<QLabel *>(label);
    return value != nullptr ? value->text() : QString();
}

PreviewPanel::Shown PreviewPanel::shown() const
{
    QWidget *current = m_preview->currentWidget();
    if (current == m_pictureLabel) {
        return Shown::Picture;
    }
    if (current == m_textView) {
        return Shown::Text;
    }
    if (current == m_markdownView) {
        return Shown::Markdown;
    }
    return m_hasItem ? Shown::Message : Shown::Nothing;
}

QString PreviewPanel::text() const
{
    QWidget *current = m_preview->currentWidget();
    if (current == m_textView) {
        return m_textView->toPlainText();
    }
    if (current == m_markdownView) {
        return m_markdownView->toPlainText();
    }
    return m_message->text();
}

} // namespace eu::ui
