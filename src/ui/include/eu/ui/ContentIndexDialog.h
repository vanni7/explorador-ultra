// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/index/ContentIndexService.h"

#include <QDialog>

class QLabel;
class QPushButton;
class QTreeWidget;

namespace eu::ui {

/// Texto corto del estado de un índice de contenido ("Leyendo 120 de 480…").
QString contentIndexStateText(const index::ContentRootStatus &status);

/// Ventana "Índice de contenido": qué carpetas tienen guardado el texto de sus
/// documentos para buscar dentro de ellos al instante (SPEC §6.5).
class ContentIndexDialog : public QDialog
{
    Q_OBJECT

public:
    /// `currentFolder`: la carpeta a la vista, para ofrecer añadirla.
    ContentIndexDialog(index::ContentIndexService &service, QString currentFolder, QWidget *parent = nullptr);

    QTreeWidget *list() const { return m_list; }

signals:
    /// Cambió la lista de carpetas (para guardarla).
    void rootsChanged();

private:
    void updateRows();
    QString selectedRoot() const;
    void add(const QString &folder);

    index::ContentIndexService &m_service;
    QString m_currentFolder;
    QTreeWidget *m_list = nullptr;
    QPushButton *m_addCurrentButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_rebuildButton = nullptr;
    QPushButton *m_pauseButton = nullptr;
};

} // namespace eu::ui
