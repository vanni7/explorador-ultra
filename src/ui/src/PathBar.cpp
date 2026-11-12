// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/PathBar.h"

#include "eu/ui/FileClipboard.h"
#include "eu/ui/Locations.h"

#include <QAbstractItemView>
#include <QClipboard>
#include <QCompleter>
#include <QDir>
#include <QFileSystemModel>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QStackedLayout>
#include <QToolButton>

namespace eu::ui {

PathBar::PathBar(QWidget *parent)
    : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::IBeamCursor);
    setToolTip(tr("Haz clic para escribir una ruta (Ctrl+L)"));

    m_crumbsPage = new QWidget(this);
    m_crumbsLayout = new QHBoxLayout(m_crumbsPage);
    m_crumbsLayout->setContentsMargins(4, 0, 4, 0);
    m_crumbsLayout->setSpacing(0);
    // La barra puede encogerse por debajo de lo que ocupan las migas: las que
    // no caben se ocultan (ver fitCrumbsToWidth).
    m_crumbsPage->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setMinimumWidth(160);

    m_edit = new QLineEdit(this);
    m_edit->setAccessibleName(tr("Ruta"));
    m_edit->installEventFilter(this);

    // Autocompletado de carpetas. QFileSystemModel lee el disco en un hilo
    // propio, así que no bloquea la interfaz.
    auto *completionModel = new QFileSystemModel(this);
    completionModel->setFilter(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Drives);
    completionModel->setRootPath(QString());
    auto *completer = new QCompleter(completionModel, this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_edit->setCompleter(completer);

    m_stack = new QStackedLayout(this);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->addWidget(m_crumbsPage);
    m_stack->addWidget(m_edit);

    connect(m_edit, &QLineEdit::returnPressed, this, [this] { finishEditing(true); });
}

QList<QPair<QString, QString>> PathBar::crumbsFor(const QString &path)
{
    QList<QPair<QString, QString>> crumbs;
    if (isVolumesLocation(path)) {
        crumbs.append({tr("Unidades"), volumesLocation()});
        return crumbs;
    }
    const QString clean = QDir::cleanPath(path);
    if (clean.isEmpty()) {
        return crumbs;
    }
    const QStringList parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);

    QString accumulated;
    if (clean.startsWith(QLatin1Char('/'))) {
        // Unix: la raíz es la primera miga.
        crumbs.append({QStringLiteral("/"), QStringLiteral("/")});
        accumulated = QString();
        for (const QString &part : parts) {
            accumulated += QLatin1Char('/') + part;
            crumbs.append({part, accumulated});
        }
    } else {
        // Windows: "C:/Users/ana" -> "C:", "Users", "ana".
        for (qsizetype i = 0; i < parts.size(); ++i) {
            accumulated += (i == 0) ? parts[i] : QLatin1Char('/') + parts[i];
            crumbs.append({parts[i], (i == 0) ? accumulated + QLatin1Char('/') : accumulated});
        }
    }
    return crumbs;
}

void PathBar::setPath(const QString &path)
{
    m_path = QDir::cleanPath(path);
    rebuildCrumbs();
    if (m_stack->currentWidget() == m_edit) {
        finishEditing(false);
    }
}

void PathBar::rebuildCrumbs()
{
    while (QLayoutItem *item = m_crumbsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_crumbWidgets.clear();

    m_ellipsis = new QLabel(QStringLiteral("… ›"), m_crumbsPage);
    m_ellipsis->setEnabled(false);
    m_crumbsLayout->addWidget(m_ellipsis);

    const QList<QPair<QString, QString>> crumbs = crumbsFor(m_path);
    for (qsizetype i = 0; i < crumbs.size(); ++i) {
        if (i > 0) {
            auto *separator = new QLabel(QStringLiteral("›"), m_crumbsPage);
            separator->setEnabled(false);
            m_crumbsLayout->addWidget(separator);
            m_crumbWidgets.append(separator);
        }
        auto *button = new QToolButton(m_crumbsPage);
        button->setText(crumbs[i].first);
        button->setAutoRaise(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(QDir::toNativeSeparators(crumbs[i].second));
        button->setFocusPolicy(Qt::TabFocus);
        if (i == crumbs.size() - 1) {
            QFont bold = button->font();
            bold.setBold(true);
            button->setFont(bold);
        }
        const QString target = crumbs[i].second;
        connect(button, &QToolButton::clicked, this, [this, target] { emit pathRequested(target); });
        m_crumbsLayout->addWidget(button);
        m_crumbWidgets.append(button);
    }
    m_crumbsLayout->addStretch();
    fitCrumbsToWidth();
}

/// Si la ruta no cabe, oculta las carpetas del principio y muestra "…":
/// lo que importa ver es dónde se está, es decir, el final de la ruta.
void PathBar::fitCrumbsToWidth()
{
    if (m_ellipsis == nullptr) {
        return;
    }
    const int available = width() - 16;
    int used = 0;
    qsizetype firstVisible = m_crumbWidgets.size();
    // Se recorre desde el final, sumando anchos, hasta que no quepa más.
    for (qsizetype i = m_crumbWidgets.size() - 1; i >= 0; --i) {
        const int needed = m_crumbWidgets[i]->sizeHint().width();
        const bool isLastCrumb = (i == m_crumbWidgets.size() - 1);
        if (!isLastCrumb && used + needed + m_ellipsis->sizeHint().width() > available) {
            break;
        }
        used += needed;
        firstVisible = i;
    }
    // No se empieza por un separador suelto.
    if (firstVisible > 0 && firstVisible < m_crumbWidgets.size()
        && qobject_cast<QLabel *>(m_crumbWidgets[firstVisible]) != nullptr) {
        ++firstVisible;
    }
    for (qsizetype i = 0; i < m_crumbWidgets.size(); ++i) {
        m_crumbWidgets[i]->setVisible(i >= firstVisible);
    }
    m_ellipsis->setVisible(firstVisible > 0);
}

void PathBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    fitCrumbsToWidth();
}

void PathBar::startEditing()
{
    m_edit->setText(QDir::toNativeSeparators(m_path));
    m_stack->setCurrentWidget(m_edit);
    m_edit->setFocus(Qt::ShortcutFocusReason);
    m_edit->selectAll();
}

void PathBar::finishEditing(bool accept)
{
    const QString typed = m_edit->text().trimmed();
    m_stack->setCurrentWidget(m_crumbsPage);
    if (accept && !typed.isEmpty()) {
        QString target = QDir::fromNativeSeparators(typed);
        if (target.startsWith(QLatin1String("~"))) {
            target = QDir::homePath() + target.mid(1);   // "~/Documentos"
        }
        emit pathRequested(QDir::cleanPath(target));
    }
}

void PathBar::mousePressEvent(QMouseEvent *event)
{
    QWidget::mousePressEvent(event);
    startEditing();
    // Al hacer clic la ruta queda seleccionada y, de paso, copiada: lo normal
    // es querer pegarla en otro sitio. Si en el portapapeles hay archivos
    // copiados o cortados se respetan: perderlos estropearía un "pegar" pendiente.
    QClipboard *clipboard = QGuiApplication::clipboard();
    // La lista de unidades no es una ruta: no hay nada útil que copiar.
    if (!isVolumesLocation(m_path) && fileSelectionFrom(clipboard->mimeData()).isEmpty()) {
        clipboard->setText(m_edit->text());
        emit pathCopied(m_edit->text());
    }
}

bool PathBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_edit) {
        if (event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            finishEditing(false);
            return true;
        }
        if (event->type() == QEvent::FocusOut && m_stack->currentWidget() == m_edit
            && !(m_edit->completer() && m_edit->completer()->popup()->isVisible())) {
            finishEditing(false);
        }
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace eu::ui
