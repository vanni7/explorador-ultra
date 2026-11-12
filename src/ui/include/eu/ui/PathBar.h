// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QWidget>

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QStackedLayout;

namespace eu::ui {

/// Barra de ruta con dos modos (SPEC §5.3):
///  - migas de pan: un botón por carpeta de la ruta; al pulsarlo se va a ella;
///  - edición: un campo de texto con autocompletado. Se entra con un clic en
///    la zona libre o con Ctrl+L, y se sale con Intro (ir) o Esc (cancelar).
class PathBar : public QWidget
{
    Q_OBJECT

public:
    explicit PathBar(QWidget *parent = nullptr);

    void setPath(const QString &path);
    QString path() const { return m_path; }

    /// Divide una ruta en migas: pares (texto visible, ruta completa).
    /// Es pública para poder probarla.
    static QList<QPair<QString, QString>> crumbsFor(const QString &path);

public slots:
    void startEditing();

signals:
    /// El usuario pidió ir a esta ruta.
    void pathRequested(const QString &path);
    /// La ruta se copió al portapapeles al hacer clic sobre ella.
    void pathCopied(const QString &nativePath);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuildCrumbs();
    void fitCrumbsToWidth();
    void finishEditing(bool accept);

    QString m_path;
    QStackedLayout *m_stack = nullptr;
    QWidget *m_crumbsPage = nullptr;
    QHBoxLayout *m_crumbsLayout = nullptr;
    QLineEdit *m_edit = nullptr;
    QLabel *m_ellipsis = nullptr;        ///< "…" visible cuando no caben todas las migas.
    QList<QWidget *> m_crumbWidgets;     ///< Botones y separadores, de izquierda a derecha.
};

} // namespace eu::ui
