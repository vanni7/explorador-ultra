// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/PreviewPanel.h"

#include "eu/archive/Archive.h"
#include "eu/content/TextExtractor.h"
#include "eu/meta/Metadata.h"
#include "eu/thumbs/Isolated.h"
#include "eu/thumbs/Thumbnail.h"
#include "eu/ui/Locations.h"
#include "eu/ui/MediaPreview.h"

#include <QAbstractTableModel>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStringDecoder>
#include <QTabBar>
#include <QTableView>
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

/// Muestra el HTML de un documento. Solo carga las imágenes que trae el
/// propio documento (ya reducidas en el proceso auxiliar): nada del disco ni
/// de la red, y los enlaces no se abren.
class PreviewDocumentView : public QTextBrowser
{
public:
    explicit PreviewDocumentView(QWidget *parent) : QTextBrowser(parent)
    {
        setOpenLinks(false);
        setOpenExternalLinks(false);
    }

    void setImages(const QList<QByteArray> &images)
    {
        m_images.clear();
        for (qsizetype i = 0; i < images.size(); ++i) {
            m_images.insert(QStringLiteral("eu-imagen:%1").arg(i), QImage::fromData(images.at(i)));
        }
    }

protected:
    QVariant loadResource(int type, const QUrl &name) override
    {
        if (type == QTextDocument::ImageResource) {
            const auto image = m_images.constFind(name.toString());
            if (image != m_images.cend()) {
                return *image;
            }
        }
        return {};
    }

private:
    QHash<QString, QImage> m_images;
};

/// Una hoja de cálculo para la cuadrícula: columnas A, B, C… y el número de
/// cada fila; los números a la derecha.
class PreviewSheetModel : public QAbstractTableModel
{
public:
    using QAbstractTableModel::QAbstractTableModel;

    void setSheet(const content::SheetPreview &sheet)
    {
        beginResetModel();
        m_sheet = sheet;
        endResetModel();
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : int(m_sheet.cells.size());
    }

    int columnCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : m_sheet.columns;
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (!index.isValid() || index.row() >= m_sheet.cells.size()) {
            return {};
        }
        const QStringList &row = m_sheet.cells.at(index.row());
        const QString text = index.column() < row.size() ? row.at(index.column()) : QString();
        if (role == Qt::DisplayRole) {
            QString line = text;
            line.replace(QLatin1Char('\n'), QLatin1Char(' '));   // en la celda, en una línea
            return line;
        }
        if (role == Qt::ToolTipRole && (text.size() > 20 || text.contains(QLatin1Char('\n')))) {
            return text;
        }
        if (role == Qt::TextAlignmentRole) {
            const QByteArray flags = m_sheet.numeric.value(index.row());
            const bool number = index.column() < flags.size() && flags.at(index.column()) == '1';
            return QVariant::fromValue(Qt::Alignment((number ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter));
        }
        return {};
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override
    {
        if (role != Qt::DisplayRole) {
            return QAbstractTableModel::headerData(section, orientation, role);
        }
        if (orientation == Qt::Horizontal) {
            return content::spreadsheetColumnName(section + 1);
        }
        return section < m_sheet.rowNumbers.size() ? QString::number(m_sheet.rowNumbers.at(section)) : QString();
    }

    const content::SheetPreview &sheet() const { return m_sheet; }

private:
    content::SheetPreview m_sheet;
};

struct PreviewPanel::Loaded
{
    QList<QPair<QString, QList<QPair<QString, QString>>>> sections;   ///< Título y campos.
    QImage picture;
    QString text;
    bool truncated = false;
    bool markdown = false;
    bool hasDocument = false;
    content::DocumentPreview document;
    QString problem;
    /// Un video o un audio: el archivo que se reproduce (el extraído, si
    /// venía de un comprimido). La miniatura, si la hay, va en `picture`.
    thumbs::MediaKind media = thumbs::MediaKind::None;
    QString mediaPath;
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
    m_documentView = new PreviewDocumentView(m_preview);
    m_documentView->setAccessibleName(tr("Documento"));
    m_sheetView = new QWidget(m_preview);
    m_sheetTabs = new QTabBar(m_sheetView);
    m_sheetTabs->setDocumentMode(true);
    m_sheetTabs->setExpanding(false);
    m_sheetTabs->setUsesScrollButtons(true);
    m_sheetTabs->setAccessibleName(tr("Hojas"));
    m_sheetModel = new PreviewSheetModel(this);
    m_sheetTable = new QTableView(m_sheetView);
    m_sheetTable->setModel(m_sheetModel);
    m_sheetTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_sheetTable->setWordWrap(false);
    m_sheetTable->setAccessibleName(tr("Celdas de la hoja"));
    m_sheetTable->verticalHeader()->setDefaultSectionSize(m_sheetTable->fontMetrics().height() + 6);
    m_sheetNote = new QLabel(m_sheetView);
    m_sheetNote->setWordWrap(true);
    m_sheetNote->setEnabled(false);
    auto *sheetLayout = new QVBoxLayout(m_sheetView);
    sheetLayout->setContentsMargins(0, 0, 0, 0);
    sheetLayout->setSpacing(2);
    sheetLayout->addWidget(m_sheetTabs);
    sheetLayout->addWidget(m_sheetTable, 1);
    sheetLayout->addWidget(m_sheetNote);
    connect(m_sheetTabs, &QTabBar::currentChanged, this, &PreviewPanel::showSheet);
    if (MediaPreview::isAvailable()) {
        m_media = new MediaPreview(m_preview);
        connect(m_media, &MediaPreview::playbackStarted, this, &PreviewPanel::mediaStarted);
        connect(m_media, &MediaPreview::volumeChanged, this, &PreviewPanel::mediaVolumeChanged);
    }
    m_message = new QLabel(m_preview);
    m_message->setAlignment(Qt::AlignCenter);
    m_message->setWordWrap(true);
    m_message->setEnabled(false);
    m_preview->addWidget(m_pictureLabel);
    m_preview->addWidget(m_textView);
    m_preview->addWidget(m_markdownView);
    m_preview->addWidget(m_documentView);
    m_preview->addWidget(m_sheetView);
    if (m_media != nullptr) {
        m_preview->addWidget(m_media);
    }
    m_preview->addWidget(m_message);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->addWidget(m_title);
    layout->addWidget(m_fieldsArea, 2);
    layout->addWidget(m_preview, 3);

    m_delay.setSingleShot(true);
    m_delay.setInterval(120);
    connect(&m_delay, &QTimer::timeout, this, &PreviewPanel::startLoading);
    m_clearDelay.setSingleShot(true);
    m_clearDelay.setInterval(1500);
    connect(&m_clearDelay, &QTimer::timeout, this, [this] { clearNow(m_pendingClearMessage); });
    clearNow(QString());
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
    // Con algo sonando, la selección puede perderse solo un instante (la
    // carpeta se vuelve a leer porque cambió algo en ella): se espera un poco
    // antes de cortarlo, por si vuelve a quedar seleccionado lo mismo.
    if (m_media != nullptr && m_hasItem && m_preview->currentWidget() == m_media && m_media->isPlaying()) {
        m_pendingClearMessage = message;
        if (!m_clearDelay.isActive()) {
            m_clearDelay.start();
        }
        return;
    }
    clearNow(message);
}

void PreviewPanel::clearNow(const QString &message)
{
    m_clearDelay.stop();
    ++m_generation;
    if (m_task.isValid()) {
        m_task.cancel();
        m_task = tasks::TaskHandle();
    }
    m_delay.stop();
    releaseMedia();
    m_hasItem = false;
    m_loading = false;
    m_picture = QImage();
    m_sections.clear();
    m_title->clear();
    rebuildFields({});
    m_message->setText(message.isEmpty() ? tr("Selecciona un archivo para ver aquí sus datos y su contenido")
                                         : message);
    m_preview->setCurrentWidget(m_message);
}

void PreviewPanel::showItem(const PreviewItem &item)
{
    m_clearDelay.stop();
    // El mismo archivo, sin cambios (la carpeta se volvió a leer, cambió
    // cuántos hay seleccionados o las unidades): no se vuelve a leer, así no
    // parpadea y lo que suena sigue sonando. Solo se rehacen los datos.
    if (m_hasItem && !m_loading && item.path == m_item.path && item.entry.size == m_item.entry.size
        && item.entry.modifiedMs == m_item.entry.modifiedMs && item.entry.isDirectory == m_item.entry.isDirectory) {
        m_item = item;
        rebuildFields(m_sections);
        return;
    }
    ++m_generation;
    if (m_task.isValid()) {
        m_task.cancel();
        m_task = tasks::TaskHandle();
    }
    releaseMedia();
    m_item = item;
    m_hasItem = true;
    m_picture = QImage();
    m_sections.clear();
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
        fitFields();
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
        auto *name = new QLabel(label + QLatin1Char(':'));
        m_fields->addRow(name, valueLabel);
        // Visibles ya (si no, Qt las muestra después y no cuentan al medir la zona).
        name->show();
        valueLabel->show();
    };
    const auto addHeading = [this](const QString &text) {
        auto *heading = new QLabel(text);
        QFont font = heading->font();
        font.setBold(true);
        heading->setFont(font);
        m_fields->addRow(heading);
        heading->show();
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
    fitFields();
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
            // Video y audio se reproducen aquí, con sonido; la miniatura, de portada.
            if (!item.entry.isDirectory && MediaPreview::isAvailable()) {
                loaded.media = thumbs::mediaKindFor(item.entry.name);
                if (loaded.media != thumbs::MediaKind::None) {
                    loaded.mediaPath = item.path;
                }
            }
            const auto post = [&self, generation](const Loaded &result) {
                QMetaObject::invokeMethod(qApp, [self, generation, result] {
                    if (self) {
                        self->onLoaded(generation, result);
                    }
                }, Qt::QueuedConnection);
            };
            // Word, Excel y OpenDocument: con su formato, leídos en el proceso
            // auxiliar (sin LibreOffice, así que también en Windows).
            if (!item.entry.isDirectory && content::hasDocumentPreview(item.entry.name)) {
                const core::Result<content::DocumentPreview> document =
                    thumbs::documentPreviewIsolated(item.path, cancel);
                if (document.ok()) {
                    loaded.hasDocument = true;
                    loaded.document = document.value();
                    post(loaded);
                    return;
                }
                if (document.error().code == core::ErrorCode::Cancelled || cancel.isCancelled()) {
                    return;
                }
                // Si no se pudo, se intenta como imagen (LibreOffice) o como texto.
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
                    const QByteArray bytes = file.read(textBytes);
                    loaded.truncated = file.size() > textBytes;
                    QStringDecoder utf8(QStringDecoder::Utf8);
                    loaded.text = utf8.decode(bytes);
                    if (utf8.hasError()) {
                        loaded.text = QString::fromLatin1(bytes);
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
            // Un documento sin imagen (un PowerPoint sin LibreOffice, un RTF…): su texto.
            const content::TextSource textSource = content::textSourceFor(item.entry.name);
            if (loaded.picture.isNull() && loaded.text.isNull() && !item.entry.isDirectory && !cancel.isCancelled()
                && textSource != content::TextSource::None && textSource != content::TextSource::Plain) {
                const core::Result<QString> text = thumbs::extractTextIsolated(item.path, cancel);
                if (text.ok() && !text.value().trimmed().isEmpty()) {
                    loaded.text = text.value().left(textBytes);
                    loaded.text.replace(QLatin1Char('\f'), QStringLiteral("\n\n"));   // páginas y diapositivas
                    loaded.truncated = text.value().size() > textBytes;
                    loaded.problem.clear();
                }
            }
            if (cancel.isCancelled()) {
                return;
            }
            post(loaded);
        });
}

void PreviewPanel::onLoaded(quint64 generation, const Loaded &loaded)
{
    if (generation != m_generation) {
        return;   // la selección cambió mientras se leía
    }
    m_loading = false;
    m_task = tasks::TaskHandle();
    m_sections = loaded.sections;
    rebuildFields(loaded.sections);
    if (loaded.media != thumbs::MediaKind::None && m_media != nullptr) {
        m_picture = loaded.picture;
        m_picture.setDevicePixelRatio(1.0);
        m_media->setMedia(loaded.mediaPath, loaded.media == thumbs::MediaKind::Audio, m_picture);
        m_preview->setCurrentWidget(m_media);
    } else if (loaded.hasDocument) {
        showDocument(loaded.document);
    } else if (!loaded.picture.isNull()) {
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

void PreviewPanel::showDocument(const content::DocumentPreview &document)
{
    m_document = document;
    if (!m_document.isSpreadsheet()) {
        m_documentView->setImages(m_document.images);
        m_documentView->setHtml(m_document.html);
        m_preview->setCurrentWidget(m_documentView);
        return;
    }
    {
        const QSignalBlocker blocker(m_sheetTabs);
        while (m_sheetTabs->count() > 0) {
            m_sheetTabs->removeTab(0);
        }
        for (const content::SheetPreview &sheet : std::as_const(m_document.sheets)) {
            m_sheetTabs->addTab(sheet.name);
        }
        m_sheetTabs->setCurrentIndex(0);
    }
    showSheet(0);
    m_preview->setCurrentWidget(m_sheetView);
}

void PreviewPanel::showSheet(int index)
{
    if (index < 0 || index >= m_document.sheets.size()) {
        return;
    }
    const content::SheetPreview &sheet = m_document.sheets.at(index);
    m_sheetModel->setSheet(sheet);
    // Columnas a la medida de lo que llevan, sin que una muy larga se coma el panel.
    m_sheetTable->resizeColumnsToContents();
    for (int column = 0; column < sheet.columns; ++column) {
        m_sheetTable->setColumnWidth(column, std::clamp(m_sheetTable->columnWidth(column), 40, 160));
    }
    QStringList notes;
    if (sheet.cells.isEmpty()) {
        notes.append(tr("Hoja vacía."));
    }
    if (sheet.moreRows) {
        notes.append(sheet.totalRows > 0
                         ? tr("Se muestran las primeras %1 filas con datos (la hoja llega hasta la fila %2).")
                               .arg(sheet.rowNumbers.size())
                               .arg(sheet.totalRows)
                         : tr("Se muestran las primeras %1 filas con datos.").arg(sheet.rowNumbers.size()));
    }
    if (sheet.moreColumns) {
        notes.append(tr("Se muestran las columnas hasta la %1.").arg(content::spreadsheetColumnName(sheet.columns)));
    }
    if (m_document.moreSheets > 0) {
        notes.append(tr("%n hoja(s) más sin mostrar.", nullptr, m_document.moreSheets));
    }
    m_sheetNote->setText(notes.join(QLatin1Char(' ')));
    m_sheetNote->setVisible(!notes.isEmpty());
}

QStringList PreviewPanel::sheetNames() const
{
    QStringList names;
    for (const content::SheetPreview &sheet : m_document.sheets) {
        names.append(sheet.name);
    }
    return names;
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

void PreviewPanel::fitFields()
{
    // Los datos ocupan lo que necesitan (como mucho la mitad): el resto, para la vista previa.
    // Con el ancho de ahora (los valores largos ocupan varias líneas); si aún
    // no se puede saber, lo que pide el contenido sin ajustar.
    const int width = m_fieldsArea->viewport()->width();
    int content = m_fieldsHost->hasHeightForWidth() && width > 50 ? m_fieldsHost->heightForWidth(width) : -1;
    content = std::max(content, m_fieldsHost->sizeHint().height());
    const int wanted = content + 2 * m_fieldsArea->frameWidth() + 2;
    m_fieldsArea->setMaximumHeight(std::max(80, std::min(wanted, height() / 2)));
}

void PreviewPanel::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    fitFields();
    updatePicture();
}

bool PreviewPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_pictureLabel && event->type() == QEvent::Resize) {
        updatePicture();
    }
    return QWidget::eventFilter(watched, event);
}

void PreviewPanel::pauseMedia()
{
    if (m_media != nullptr) {
        m_media->pause();
    }
}

void PreviewPanel::releaseMedia()
{
    if (m_media != nullptr) {
        m_media->release();
    }
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
    if (current == m_documentView) {
        return Shown::Document;
    }
    if (current == m_sheetView) {
        return Shown::Spreadsheet;
    }
    if (current == m_media && m_media != nullptr) {
        return Shown::Media;
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
    if (current == m_documentView) {
        return m_documentView->toPlainText();
    }
    if (current == m_sheetView) {
        QStringList lines;
        for (const QStringList &row : m_sheetModel->sheet().cells) {
            lines.append(row.join(QLatin1Char('\t')));
        }
        return lines.join(QLatin1Char('\n'));
    }
    return m_message->text();
}

} // namespace eu::ui
