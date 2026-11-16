// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/similar/Similar.h"
#include "eu/tasks/Scheduler.h"

#include <QDialog>
#include <QHash>
#include <QSet>

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;

namespace eu::ui {

/// Ventana "Imágenes parecidas" (SPEC §14.2): grupos de fotos parecidas y,
/// del elegido, las imágenes lado a lado con resolución, tamaño, fecha y
/// parecido. Solo se quita lo que se marca a mano, tras confirmar, a la
/// papelera propia; nunca todas las de un grupo.
class SimilarImagesDialog : public QDialog
{
    Q_OBJECT

public:
    SimilarImagesDialog(tasks::Scheduler &scheduler, QString folder, QString volumeOfFolder, QStringList allVolumes,
                        QWidget *parent = nullptr);
    ~SimilarImagesDialog() override;

    QStringList marked() const { return QStringList(m_marked.constBegin(), m_marked.constEnd()); }

    // Para las pruebas.
    void startSearch();
    bool isSearching() const { return m_searching; }
    int groupCount() const { return int(m_groups.size()); }
    const QList<similar::Group> &groups() const { return m_groups; }
    /// Marca o desmarca; false si no se puede (sería la última del grupo).
    bool setMarked(const QString &path, bool marked);
    void setConfirmForTesting(std::function<bool(int files, qint64 bytes)> confirm) { m_confirm = std::move(confirm); }
    void sendToTrash();

signals:
    void searchFinished();

private:
    void showResult(quint64 generation, const similar::Result &result);
    void showGroup(int index);
    void updateSummary();

    tasks::Scheduler &m_scheduler;
    QString m_folder;
    QString m_volume;
    QStringList m_allVolumes;
    QList<similar::Group> m_groups;
    QSet<QString> m_marked;
    tasks::TaskHandle m_task;
    tasks::TaskHandle m_cardsTask;
    quint64 m_generation = 0;
    quint64 m_cardsGeneration = 0;
    bool m_searching = false;
    std::function<bool(int, qint64)> m_confirm;

    QComboBox *m_scope = nullptr;
    QSlider *m_threshold = nullptr;
    QLabel *m_thresholdLabel = nullptr;
    QPushButton *m_searchButton = nullptr;
    QLabel *m_progress = nullptr;
    QListWidget *m_groupList = nullptr;
    QWidget *m_cards = nullptr;
    QHBoxLayout *m_cardsLayout = nullptr;
    QHash<QString, QLabel *> m_cardImages;
    QHash<QString, QLabel *> m_cardDetails;
    QHash<QString, QCheckBox *> m_cardChecks;
    QLabel *m_summary = nullptr;
    QPushButton *m_trashButton = nullptr;
};

} // namespace eu::ui
