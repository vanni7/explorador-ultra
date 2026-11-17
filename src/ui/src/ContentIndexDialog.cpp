// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/ContentIndexDialog.h"

#include "eu/core/SizeFormat.h"

#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace eu::ui {

namespace {

enum Column { FolderColumn, StateColumn, DocumentsColumn, ColumnCount };
const int RootRole = Qt::UserRole + 1;

} // namespace

QString contentIndexStateText(const index::ContentRootStatus &status)
{
    using State = index::ContentRootStatus::State;
    const QLocale locale;
    switch (status.state) {
    case State::Building:
    case State::Updating:
        if (status.progress.toRead > 0) {
            return ContentIndexDialog::tr("Leyendo documentos: %1 de %2")
                .arg(locale.toString(status.progress.read), locale.toString(status.progress.toRead));
        }
        return status.state == State::Building
            ? ContentIndexDialog::tr("Buscando documentos… %1").arg(locale.toString(status.progress.found))
            : ContentIndexDialog::tr("Comprobando cambios… %1").arg(locale.toString(status.progress.found));
    case State::Waiting:
        return ContentIndexDialog::tr("En espera");
    case State::Disconnected:
        return ContentIndexDialog::tr("No está conectada (se conserva el índice)");
    case State::Failed:
        return ContentIndexDialog::tr("Error: %1").arg(status.error);
    case State::Ready:
        break;
    }
    if (status.info.updatedMs > 0) {
        return ContentIndexDialog::tr("Al día (repasado %1)")
            .arg(locale.toString(QDateTime::fromMSecsSinceEpoch(status.info.updatedMs), QLocale::ShortFormat));
    }
    return ContentIndexDialog::tr("Al día");
}

ContentIndexDialog::ContentIndexDialog(index::ContentIndexService &service, QString currentFolder, QWidget *parent)
    : QDialog(parent), m_service(service), m_currentFolder(std::move(currentFolder))
{
    setWindowTitle(tr("Índice de contenido"));
    resize(780, 420);

    auto *intro = new QLabel(
        tr("Guarda el texto de los documentos (PDF, Word, Excel, PowerPoint, OpenDocument, RTF y texto) de las "
           "carpetas que elijas, para que buscar dentro de ellos sea casi instantáneo en vez de leerlos uno a uno. "
           "Se crea en segundo plano y se guarda solo en este equipo. Es mejor elegir carpetas de documentos (una "
           "biblioteca, los papeles de la casa) que unidades enteras: guardar el texto ocupa espacio. Lo que "
           "cambie después se lee del disco al buscar hasta el siguiente repaso, así que nunca da resultados viejos."),
        this);
    intro->setWordWrap(true);

    m_list = new QTreeWidget(this);
    m_list->setColumnCount(ColumnCount);
    m_list->setHeaderLabels({tr("Carpeta"), tr("Estado"), tr("Documentos")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAccessibleName(tr("Carpetas con índice de contenido"));
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(FolderColumn, QHeaderView::Stretch);
    m_list->header()->resizeSection(StateColumn, 260);
    m_list->header()->resizeSection(DocumentsColumn, 150);

    auto *addButton = new QPushButton(tr("Añadir carpeta…"), this);
    m_addCurrentButton = new QPushButton(this);
    m_removeButton = new QPushButton(tr("Quitar"), this);
    m_removeButton->setToolTip(tr("Deja de indexar la carpeta seleccionada y borra lo guardado"));
    m_refreshButton = new QPushButton(tr("Actualizar ahora"), this);
    m_refreshButton->setToolTip(tr("Busca documentos nuevos o cambiados en la carpeta seleccionada (o en todas)"));
    m_rebuildButton = new QPushButton(tr("Reconstruir"), this);
    m_rebuildButton->setToolTip(tr("Borra lo guardado de la carpeta seleccionada y lee de nuevo todos sus "
                                   "documentos (también los que no se pudieron leer)"));
    m_pauseButton = new QPushButton(this);
    auto *closeButton = new QPushButton(tr("Cerrar"), this);
    closeButton->setDefault(true);

    auto *top = new QHBoxLayout;
    top->addWidget(addButton);
    top->addWidget(m_addCurrentButton);
    top->addStretch(1);
    auto *bottom = new QHBoxLayout;
    bottom->addWidget(m_removeButton);
    bottom->addWidget(m_refreshButton);
    bottom->addWidget(m_rebuildButton);
    bottom->addWidget(m_pauseButton);
    bottom->addStretch(1);
    bottom->addWidget(closeButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addLayout(top);
    layout->addWidget(m_list, 1);
    layout->addLayout(bottom);

    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(addButton, &QPushButton::clicked, this, [this] {
        const QString folder = QFileDialog::getExistingDirectory(this, tr("Carpeta de documentos que indexar"),
                                                                 m_currentFolder);
        if (!folder.isEmpty()) {
            add(folder);
        }
    });
    connect(m_addCurrentButton, &QPushButton::clicked, this, [this] { add(m_currentFolder); });
    connect(m_removeButton, &QPushButton::clicked, this, [this] {
        const QString root = selectedRoot();
        if (!root.isEmpty()) {
            m_service.removeRoot(root);
            emit rootsChanged();
        }
    });
    connect(m_refreshButton, &QPushButton::clicked, this, [this] {
        const QString selected = selectedRoot();
        for (const QString &root : selected.isEmpty() ? m_service.roots() : QStringList{selected}) {
            m_service.refresh(root);
        }
    });
    connect(m_rebuildButton, &QPushButton::clicked, this, [this] {
        const QString root = selectedRoot();
        if (!root.isEmpty()) {
            m_service.rebuild(root);
        }
    });
    connect(m_pauseButton, &QPushButton::clicked, this, [this] { m_service.setPaused(!m_service.isPaused()); });
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &ContentIndexDialog::updateRows);
    connect(&m_service, &index::ContentIndexService::changed, this, &ContentIndexDialog::updateRows);
    updateRows();
}

void ContentIndexDialog::add(const QString &folder)
{
    const QString clean = QDir::cleanPath(folder);
    for (const QString &root : m_service.roots()) {
        const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
        if (clean == root || clean.startsWith(prefix)) {
            QMessageBox::information(this, windowTitle(),
                                     tr("«%1» ya está dentro de una carpeta indexada.").arg(QDir::toNativeSeparators(clean)));
            return;
        }
    }
    m_service.addRoot(clean);
    emit rootsChanged();
}

QString ContentIndexDialog::selectedRoot() const
{
    const QList<QTreeWidgetItem *> selected = m_list->selectedItems();
    return selected.isEmpty() ? QString() : selected.first()->data(FolderColumn, RootRole).toString();
}

void ContentIndexDialog::updateRows()
{
    const QString selected = selectedRoot();
    const QList<index::ContentRootStatus> all = m_service.status();
    // Las filas se rehacen solo si cambió la lista; si no, se actualizan.
    QStringList listed;
    for (int row = 0; row < m_list->topLevelItemCount(); ++row) {
        listed.append(m_list->topLevelItem(row)->data(FolderColumn, RootRole).toString());
    }
    QStringList wanted;
    for (const index::ContentRootStatus &status : all) {
        wanted.append(status.root);
    }
    if (listed != wanted) {
        const QSignalBlocker blocker(m_list);
        m_list->clear();
        for (const index::ContentRootStatus &status : all) {
            auto *item = new QTreeWidgetItem(m_list);
            item->setData(FolderColumn, RootRole, status.root);
            item->setText(FolderColumn, QDir::toNativeSeparators(status.root));
            item->setToolTip(FolderColumn, QDir::toNativeSeparators(status.root));
            if (status.root == selected) {
                item->setSelected(true);
            }
        }
    }
    const QLocale locale;
    for (int row = 0; row < m_list->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = m_list->topLevelItem(row);
        const index::ContentRootStatus &status = all.at(row);
        item->setText(StateColumn, contentIndexStateText(status));
        item->setToolTip(StateColumn, contentIndexStateText(status));
        const qint64 withoutText = status.info.documents - status.info.withText;
        item->setText(DocumentsColumn, status.info.documents > 0 ? locale.toString(status.info.documents) : QString());
        item->setToolTip(DocumentsColumn,
                         withoutText > 0
                             ? tr("%1 con texto; %2 sin texto (escaneados, protegidos o dañados)")
                                   .arg(locale.toString(status.info.withText), locale.toString(withoutText))
                             : QString());
    }

    const bool hasSelection = !selectedRoot().isEmpty();
    m_removeButton->setEnabled(hasSelection);
    m_rebuildButton->setEnabled(hasSelection);
    m_refreshButton->setEnabled(!all.isEmpty());
    m_refreshButton->setText(hasSelection ? tr("Actualizar ahora") : tr("Actualizar todas"));
    m_pauseButton->setText(m_service.isPaused() ? tr("Reanudar") : tr("Pausar"));
    m_pauseButton->setEnabled(m_service.isPaused() || m_service.isBusy());
    m_addCurrentButton->setText(tr("Añadir «%1»").arg(QFileInfo(m_currentFolder).fileName().isEmpty()
                                                         ? QDir::toNativeSeparators(m_currentFolder)
                                                         : QFileInfo(m_currentFolder).fileName()));
    m_addCurrentButton->setToolTip(QDir::toNativeSeparators(m_currentFolder));
    m_addCurrentButton->setVisible(!m_currentFolder.isEmpty() && QFileInfo(m_currentFolder).isDir()
                                   && !m_service.roots().contains(QDir::cleanPath(m_currentFolder)));
}

} // namespace eu::ui
