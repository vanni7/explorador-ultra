// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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
class QTextBrowser;

namespace eu::ui {

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
/// nada del archivo: los documentos se ven como imagen y los textos como texto.
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
    enum class Shown { Nothing, Picture, Text, Markdown, Message };
    Shown shown() const;
    bool isLoading() const { return m_loading; }
    QImage picture() const { return m_picture; }
    QString text() const;

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    struct Loaded;
    void startLoading();
    void onLoaded(quint64 generation, const Loaded &loaded);
    void rebuildFields(const QList<QPair<QString, QList<QPair<QString, QString>>>> &extra);
    void updatePicture();

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
    QLabel *m_message = nullptr;
};

} // namespace eu::ui
