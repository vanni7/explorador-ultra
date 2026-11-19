// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/content/DocumentPreview.h"
#include "eu/core/SizeFormat.h"
#include "eu/fs/Entry.h"
#include "eu/tasks/Scheduler.h"

#include <QImage>
#include <QTimer>
#include <QWidget>

class QFormLayout;
class QLabel;
class QPlainTextEdit;
class QScrollArea;
class QStackedWidget;
class QTabBar;
class QTableView;
class QTextBrowser;

namespace eu::ui {

class PreviewDocumentView;
class PreviewSheetModel;

/// Lo que el panel necesita saber del elemento a la vista.
struct PreviewItem
{
    QString path;
    fs::Entry entry;
    QString typeText;    ///< "imagen JPEG", "Carpeta"…, tal como lo dice la lista.
    int selectedCount = 1;
};

/// Panel derecho (SPEC §5.3 y §7): metadatos arriba y vista previa abajo,
/// según el tipo de archivo.
///
/// Todo lo que lee el disco se hace en una tarea; si la selección cambia
/// antes de que acabe, lo anterior se cancela y se descarta. Nunca ejecuta
/// nada del archivo: Word y OpenDocument se ven con su formato (leídos en el
/// proceso auxiliar), las hojas de cálculo como cuadrícula, otros documentos
/// como imagen (o, si no se puede, su texto) y los textos como texto.
class PreviewPanel : public QWidget
{
    Q_OBJECT

public:
    PreviewPanel(tasks::Scheduler &scheduler, QWidget *parent = nullptr);
    ~PreviewPanel() override;

    void showItem(const PreviewItem &item);
    /// Nada seleccionado (o una ubicación sin archivos, como "Unidades").
    void clear(const QString &message = QString());
    void setSizeUnits(core::SizeUnits units);

    /// Para las pruebas: lo que se está mostrando.
    QString title() const;
    QString fieldValue(const QString &label) const;
    enum class Shown { Nothing, Picture, Text, Markdown, Document, Spreadsheet, Message };
    Shown shown() const;
    bool isLoading() const { return m_loading; }
    QImage picture() const { return m_picture; }
    /// El texto a la vista; de una hoja de cálculo, sus celdas (tabulador entre
    /// columnas, una fila por línea).
    QString text() const;
    QStringList sheetNames() const;
    void showSheet(int index);

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Loaded;
    void startLoading();
    void onLoaded(quint64 generation, const Loaded &loaded);
    void rebuildFields(const QList<QPair<QString, QList<QPair<QString, QString>>>> &extra);
    void updatePicture();
    void showDocument(const content::DocumentPreview &document);

    tasks::Scheduler &m_scheduler;
    core::SizeUnits m_units = core::SizeUnits::Decimal;
    PreviewItem m_item;
    bool m_hasItem = false;
    quint64 m_generation = 0;
    tasks::TaskHandle m_task;
    QTimer m_delay;   ///< Al recorrer la lista con las flechas, no se lee cada archivo.
    bool m_loading = false;
    QImage m_picture;

    QLabel *m_title = nullptr;
    QScrollArea *m_fieldsArea = nullptr;
    QWidget *m_fieldsHost = nullptr;
    QFormLayout *m_fields = nullptr;
    QStackedWidget *m_preview = nullptr;
    QLabel *m_pictureLabel = nullptr;
    QPlainTextEdit *m_textView = nullptr;
    QTextBrowser *m_markdownView = nullptr;
    PreviewDocumentView *m_documentView = nullptr;
    QWidget *m_sheetView = nullptr;
    QTabBar *m_sheetTabs = nullptr;
    QTableView *m_sheetTable = nullptr;
    PreviewSheetModel *m_sheetModel = nullptr;
    QLabel *m_sheetNote = nullptr;
    content::DocumentPreview m_document;   ///< El documento a la vista (sus hojas, al cambiar de pestaña).
    QLabel *m_message = nullptr;
};

} // namespace eu::ui
