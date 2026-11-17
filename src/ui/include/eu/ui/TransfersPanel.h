// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/ops/Engine.h"
#include "eu/ui/FileActions.h"

#include <QDialog>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace eu::ui {

/// "1 h 5 min", "3 min 20 s", "12 s".
QString formatDuration(qint64 seconds);

/// Ventana "Ya existe…": qué hacer con un choque de nombres (SPEC §13).
class CollisionDialog : public QDialog
{
    Q_OBJECT

public:
    CollisionDialog(const ops::Conflict &conflict, bool moving, QWidget *parent = nullptr);
    ops::CollisionDecision decision() const { return m_decision; }

private:
    ops::CollisionDecision m_decision;
    QCheckBox *m_forAll = nullptr;
};

/// Panel "Transferencias": la cola de copias y movimientos con su avance,
/// velocidad y tiempo restante, y los botones para pausar, cancelar,
/// reintentar o adelantar cada una (SPEC §13).
class TransfersPanel : public QWidget
{
    Q_OBJECT

public:
    explicit TransfersPanel(FileActions &actions, QWidget *parent = nullptr);

    QTreeWidget *list() const { return m_list; }

    /// Opciones de copia elegidas aquí (y guardadas en la configuración).
    static ops::CopyOptions copyOptionsFromSettings();

private:
    void updateRows();
    quint64 selectedId() const;

    FileActions &m_actions;
    QLabel *m_summary = nullptr;
    QTreeWidget *m_list = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QPushButton *m_retryButton = nullptr;
    QPushButton *m_firstButton = nullptr;
    QPushButton *m_clearButton = nullptr;
    QCheckBox *m_sequential = nullptr;
    QCheckBox *m_verify = nullptr;
    QComboBox *m_algorithm = nullptr;
};

} // namespace eu::ui
