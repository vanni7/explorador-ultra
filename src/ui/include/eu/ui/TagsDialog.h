// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/tags/TagStore.h"
#include "eu/tasks/Scheduler.h"

#include <QDialog>

class QLabel;
class QListWidget;
class QPushButton;
class QTreeWidget;

namespace eu::ui {

/// Un icono redondo del color de una etiqueta.
QIcon tagIcon(const QString &color);

/// Ventana "Etiquetas" (SPEC §17): crear, renombrar, cambiar el color y
/// borrar etiquetas, y revisar las huérfanas (archivos que ya no se
/// encuentran): buscarlas de nuevo o quitarlas. Nunca se reasignan por
/// adivinanza.
class TagsDialog : public QDialog
{
    Q_OBJECT

public:
    TagsDialog(tasks::Scheduler &scheduler, std::shared_ptr<tags::TagStore> store, QStringList searchRoots,
               QWidget *parent = nullptr);
    ~TagsDialog() override;

    // Para las pruebas.
    QListWidget *tagList() const { return m_tags; }
    QTreeWidget *orphanList() const { return m_orphans; }
    void reviewOrphans();
    bool isBusy() const { return m_busy; }

signals:
    /// Cambiaron las etiquetas (para repintar las listas).
    void tagsChanged();

private:
    void reloadTags();
    void reloadOrphans(const QList<tags::Orphan> &orphans);
    void relocateSelected();

    tasks::Scheduler &m_scheduler;
    std::shared_ptr<tags::TagStore> m_store;
    QStringList m_searchRoots;
    tasks::TaskHandle m_task;
    bool m_busy = false;

    QListWidget *m_tags = nullptr;
    QTreeWidget *m_orphans = nullptr;
    QLabel *m_orphanNote = nullptr;
};

} // namespace eu::ui
