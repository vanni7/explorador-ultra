// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/TransfersPanel.h"

#include "eu/core/SizeFormat.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace eu::ui {

namespace {

enum Column { OperationColumn, StateColumn, ProgressColumn, SpeedColumn, LeftColumn, ColumnCount };
const int IdRole = Qt::UserRole + 1;

QString stateText(const TransferInfo &info)
{
    using State = TransferInfo::State;
    switch (info.state) {
    case State::Queued:    return TransfersPanel::tr("En cola");
    case State::Running:   return info.verifies ? TransfersPanel::tr("En curso (comprobando)") : TransfersPanel::tr("En curso");
    case State::Paused:    return TransfersPanel::tr("En pausa");
    case State::Finished:  return TransfersPanel::tr("Terminada");
    case State::Failed:    return TransfersPanel::tr("Con errores");
    case State::Cancelled: return TransfersPanel::tr("Cancelada");
    }
    return {};
}

QString progressText(const ops::Progress &progress)
{
    if (progress.bytesTotal > 0) {
        return TransfersPanel::tr("%1 % · %2 de %3")
            .arg(int(progress.bytesDone * 100 / progress.bytesTotal))
            .arg(core::formatSize(progress.bytesDone), core::formatSize(progress.bytesTotal));
    }
    if (progress.itemsTotal > 0) {
        return TransfersPanel::tr("%1 de %2 elementos").arg(progress.itemsDone).arg(progress.itemsTotal);
    }
    return {};
}

QString describeSide(const ops::FileStamp &stamp)
{
    if (stamp.isDirectory) {
        return CollisionDialog::tr("Carpeta");
    }
    if (stamp.isSymlink) {
        return CollisionDialog::tr("Enlace");
    }
    return CollisionDialog::tr("%1 · modificado el %2")
        .arg(core::formatSize(stamp.size),
             QLocale().toString(QDateTime::fromMSecsSinceEpoch(stamp.modifiedMs), QLocale::ShortFormat));
}

} // namespace

QString formatDuration(qint64 seconds)
{
    if (seconds < 0) {
        return {};
    }
    if (seconds >= 3600) {
        return TransfersPanel::tr("%1 h %2 min").arg(seconds / 3600).arg((seconds % 3600) / 60);
    }
    if (seconds >= 60) {
        return TransfersPanel::tr("%1 min %2 s").arg(seconds / 60).arg(seconds % 60);
    }
    return TransfersPanel::tr("%1 s").arg(seconds);
}

CollisionDialog::CollisionDialog(const ops::Conflict &conflict, bool moving, QWidget *parent) : QDialog(parent)
{
    const QFileInfo target(conflict.target);
    setWindowTitle(tr("Ya existe «%1»").arg(target.fileName()));
    auto *title = new QLabel(tr("<b>En «%1» ya hay algo llamado «%2».</b>")
                                 .arg(QDir::toNativeSeparators(target.absolutePath()).toHtmlEscaped(),
                                      target.fileName().toHtmlEscaped()),
                             this);
    title->setWordWrap(true);
    auto *grid = new QGridLayout;
    grid->addWidget(new QLabel(moving ? tr("Lo que se mueve:") : tr("Lo que se copia:"), this), 0, 0);
    grid->addWidget(new QLabel(describeSide(conflict.sourceStamp), this), 0, 1);
    grid->addWidget(new QLabel(tr("Lo que ya está:"), this), 1, 0);
    grid->addWidget(new QLabel(describeSide(conflict.targetStamp), this), 1, 1);
    const bool sameSize = !conflict.sourceStamp.isDirectory && !conflict.targetStamp.isDirectory
        && conflict.sourceStamp.size == conflict.targetStamp.size;
    auto *note = new QLabel(sameSize ? tr("Miden lo mismo, pero eso no basta para saber si son iguales: "
                                          "«Omitir si son idénticos» compara el contenido.")
                                     : QString(),
                            this);
    note->setWordWrap(true);
    note->setVisible(sameSize);

    struct Choice
    {
        QString text;
        QString tip;
        ops::CollisionPolicy policy;
    };
    const QList<Choice> choices = {
        {tr("Reemplazar"), tr("Lo que ya está va a la papelera: se puede deshacer."), ops::CollisionPolicy::Overwrite},
        {tr("Reemplazar si es más reciente"), tr("Solo si lo que llega se modificó después; si no, se omite."),
         ops::CollisionPolicy::OverwriteIfNewer},
        {tr("Mantener los dos"), tr("Lo que llega se queda con otro nombre: «%1».")
                                     .arg(ops::uniqueName(target.absolutePath(), target.fileName())),
         ops::CollisionPolicy::AutoRename},
        {tr("Añadir fecha y hora al nombre"), tr("Con la fecha del original: «%1».")
                                                  .arg(ops::nameWithDateTime(target.fileName(),
                                                                             conflict.sourceStamp.modifiedMs > 0
                                                                                 ? conflict.sourceStamp.modifiedMs
                                                                                 : QDateTime::currentMSecsSinceEpoch())),
         ops::CollisionPolicy::AppendDateTime},
        {tr("Omitir si son idénticos"), tr("Compara el contenido (SHA-256). Si son distintos, mantiene los dos."),
         ops::CollisionPolicy::SkipIfIdentical},
        {tr("Omitir"), tr("No se copia este elemento."), ops::CollisionPolicy::Skip},
    };
    auto *buttons = new QVBoxLayout;
    for (const Choice &choice : choices) {
        auto *button = new QPushButton(choice.text, this);
        button->setToolTip(choice.tip);
        // Intro elige lo que no toca nada de lo que ya está: mantener los dos.
        button->setAutoDefault(false);
        button->setDefault(choice.policy == ops::CollisionPolicy::AutoRename);
        if (choice.policy == ops::CollisionPolicy::AutoRename) {
            button->setFocus();
        }
        connect(button, &QPushButton::clicked, this, [this, policy = choice.policy] {
            m_decision.policy = policy;
            m_decision.applyToAll = m_forAll->isChecked();
            accept();
        });
        buttons->addWidget(button);
    }
    m_forAll = new QCheckBox(tr("Hacer lo mismo con los demás choques de esta operación"), this);
    auto *cancel = new QPushButton(tr("Cancelar la operación"), this);
    connect(cancel, &QPushButton::clicked, this, [this] {
        m_decision.cancel = true;
        reject();
    });
    m_decision.cancel = true;   // cerrar la ventana es cancelar
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(title);
    layout->addLayout(grid);
    layout->addWidget(note);
    layout->addLayout(buttons);
    layout->addWidget(m_forAll);
    layout->addWidget(cancel, 0, Qt::AlignRight);
    connect(this, &QDialog::accepted, this, [this] { m_decision.cancel = false; });
}

ops::CopyOptions TransfersPanel::copyOptionsFromSettings()
{
    const QSettings settings;
    ops::CopyOptions options;
    options.verify = settings.value(QStringLiteral("transferencias/comprobar"), false).toBool();
    options.algorithm = ops::HashAlgorithm(
        std::clamp(settings.value(QStringLiteral("transferencias/algoritmo"), 0).toInt(), 0, 3));
    return options;
}

TransfersPanel::TransfersPanel(FileActions &actions, QWidget *parent) : QWidget(parent), m_actions(actions)
{
    m_summary = new QLabel(this);
    m_list = new QTreeWidget(this);
    m_list->setColumnCount(ColumnCount);
    m_list->setHeaderLabels({tr("Operación"), tr("Estado"), tr("Avance"), tr("Velocidad"), tr("Queda")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAccessibleName(tr("Cola de transferencias"));
    m_list->header()->setStretchLastSection(false);
    m_list->header()->setSectionResizeMode(OperationColumn, QHeaderView::Stretch);
    m_list->header()->resizeSection(StateColumn, 150);
    m_list->header()->resizeSection(ProgressColumn, 220);
    m_list->header()->resizeSection(SpeedColumn, 100);
    m_list->header()->resizeSection(LeftColumn, 100);

    m_pauseButton = new QPushButton(tr("Pausar"), this);
    m_cancelButton = new QPushButton(tr("Cancelar"), this);
    m_retryButton = new QPushButton(tr("Reintentar"), this);
    m_retryButton->setToolTip(tr("Vuelve a intentar lo que no se terminó, en el mismo destino; lo que ya "
                                 "llegó entero no se repite"));
    m_firstButton = new QPushButton(tr("Pasar delante"), this);
    m_firstButton->setToolTip(tr("Será la siguiente en empezar"));
    m_clearButton = new QPushButton(tr("Quitar las terminadas"), this);

    m_sequential = new QCheckBox(tr("De una en una"), this);
    m_sequential->setToolTip(tr("Desmarcado, todas a la vez (nunca dos sobre el mismo disco mecánico)"));
    m_verify = new QCheckBox(tr("Comprobar cada copia con"), this);
    m_verify->setToolTip(tr("Relee cada archivo copiado y compara su hash con el del original. Más lento, "
                            "pero detecta discos o cables que corrompen datos."));
    m_algorithm = new QComboBox(this);
    for (const ops::HashAlgorithm algorithm :
         {ops::HashAlgorithm::Sha256, ops::HashAlgorithm::Sha1, ops::HashAlgorithm::Md5, ops::HashAlgorithm::Crc32}) {
        m_algorithm->addItem(ops::hashAlgorithmName(algorithm), int(algorithm));
        const QString limitation = ops::hashAlgorithmLimitation(algorithm);
        m_algorithm->setItemData(m_algorithm->count() - 1,
                                 limitation.isEmpty() ? tr("El recomendado.") : limitation, Qt::ToolTipRole);
    }

    {
        const QSettings settings;
        m_sequential->setChecked(settings.value(QStringLiteral("transferencias/unaAUna"), true).toBool());
        const ops::CopyOptions copy = copyOptionsFromSettings();
        m_verify->setChecked(copy.verify);
        m_algorithm->setCurrentIndex(int(copy.algorithm));
    }
    m_actions.setSequentialTransfers(m_sequential->isChecked());

    auto *options = new QHBoxLayout;
    options->addWidget(m_summary, 1);
    options->addWidget(m_sequential);
    options->addWidget(m_verify);
    options->addWidget(m_algorithm);
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_pauseButton);
    buttons->addWidget(m_cancelButton);
    buttons->addWidget(m_retryButton);
    buttons->addWidget(m_firstButton);
    buttons->addStretch(1);
    buttons->addWidget(m_clearButton);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addLayout(options);
    layout->addWidget(m_list, 1);
    layout->addLayout(buttons);

    connect(m_sequential, &QCheckBox::toggled, this, [this](bool checked) {
        QSettings().setValue(QStringLiteral("transferencias/unaAUna"), checked);
        m_actions.setSequentialTransfers(checked);
    });
    const auto saveVerify = [this] {
        QSettings settings;
        settings.setValue(QStringLiteral("transferencias/comprobar"), m_verify->isChecked());
        settings.setValue(QStringLiteral("transferencias/algoritmo"), m_algorithm->currentData().toInt());
        m_algorithm->setToolTip(m_algorithm->currentData(Qt::ToolTipRole).toString());
    };
    connect(m_verify, &QCheckBox::toggled, this, saveVerify);
    connect(m_algorithm, &QComboBox::currentIndexChanged, this, saveVerify);
    m_algorithm->setToolTip(m_algorithm->currentData(Qt::ToolTipRole).toString());

    connect(m_pauseButton, &QPushButton::clicked, this, [this] {
        const quint64 id = selectedId();
        for (const TransferInfo &info : m_actions.transfers()) {
            if (info.id == id) {
                if (info.state == TransferInfo::State::Paused) {
                    m_actions.resumeTransfer(id);
                } else {
                    m_actions.pauseTransfer(id);
                }
            }
        }
    });
    connect(m_cancelButton, &QPushButton::clicked, this, [this] { m_actions.cancelTransfer(selectedId()); });
    connect(m_retryButton, &QPushButton::clicked, this, [this] { m_actions.retryTransfer(selectedId()); });
    connect(m_firstButton, &QPushButton::clicked, this, [this] { m_actions.prioritizeTransfer(selectedId()); });
    connect(m_clearButton, &QPushButton::clicked, &m_actions, &FileActions::clearFinishedTransfers);
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &TransfersPanel::updateRows);
    connect(&m_actions, &FileActions::transfersChanged, this, &TransfersPanel::updateRows);
    updateRows();
}

quint64 TransfersPanel::selectedId() const
{
    const QList<QTreeWidgetItem *> selected = m_list->selectedItems();
    return selected.isEmpty() ? 0 : selected.first()->data(OperationColumn, IdRole).toULongLong();
}

void TransfersPanel::updateRows()
{
    const QList<TransferInfo> all = m_actions.transfers();
    const quint64 selected = selectedId();
    // Se rehacen las filas solo si cambió la lista o su orden.
    bool sameRows = m_list->topLevelItemCount() == all.size();
    for (int row = 0; sameRows && row < all.size(); ++row) {
        sameRows = m_list->topLevelItem(row)->data(OperationColumn, IdRole).toULongLong() == all[row].id;
    }
    if (!sameRows) {
        const QSignalBlocker blocker(m_list);
        m_list->clear();
        for (const TransferInfo &info : all) {
            auto *item = new QTreeWidgetItem(m_list);
            item->setData(OperationColumn, IdRole, info.id);
            item->setSelected(info.id == selected);
        }
    }
    TransferInfo current;
    for (int row = 0; row < all.size(); ++row) {
        const TransferInfo &info = all[row];
        QTreeWidgetItem *item = m_list->topLevelItem(row);
        item->setText(OperationColumn, info.description);
        item->setToolTip(OperationColumn, info.problem.isEmpty() ? info.description : info.problem);
        item->setText(StateColumn, stateText(info));
        item->setToolTip(StateColumn, info.problem);
        item->setText(ProgressColumn, progressText(info.progress));
        item->setText(SpeedColumn, info.bytesPerSecond > 0
                                       ? tr("%1/s").arg(core::formatSize(qint64(info.bytesPerSecond)))
                                       : QString());
        item->setText(LeftColumn, formatDuration(info.secondsLeft));
        if (info.id == selectedId()) {
            current = info;
        }
    }

    const TransferInfo total = m_actions.overallTransfer();
    QString summary;
    int active = 0;
    for (const TransferInfo &info : all) {
        active += info.isActive() ? 1 : 0;
    }
    if (active > 0) {
        summary = tr("%n activa(s)", nullptr, active);
        if (total.progress.bytesTotal > 0) {
            summary += tr(" · %1 %").arg(int(total.progress.bytesDone * 100 / total.progress.bytesTotal));
        }
        if (total.bytesPerSecond > 0) {
            summary += tr(" · %1/s").arg(core::formatSize(qint64(total.bytesPerSecond)));
        }
        if (total.secondsLeft >= 0) {
            summary += tr(" · quedan %1").arg(formatDuration(total.secondsLeft));
        }
    } else {
        summary = all.isEmpty() ? tr("No hay transferencias") : tr("Todas terminadas");
    }
    m_summary->setText(summary);

    const bool hasSelection = current.id != 0;
    m_pauseButton->setText(current.state == TransferInfo::State::Paused ? tr("Reanudar") : tr("Pausar"));
    m_pauseButton->setEnabled(hasSelection && (current.state == TransferInfo::State::Running
                                               || current.state == TransferInfo::State::Paused));
    m_cancelButton->setEnabled(hasSelection && current.isActive());
    m_retryButton->setEnabled(hasSelection && current.canRetry && !current.isActive());
    m_firstButton->setEnabled(hasSelection && current.state == TransferInfo::State::Queued);
    m_clearButton->setEnabled(all.size() > active);
}

} // namespace eu::ui
