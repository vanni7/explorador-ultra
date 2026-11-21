// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/IndexDialog.h"

#include "eu/ui/VolumesView.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>

namespace eu::ui {

namespace {

enum Column { NameColumn, LocationColumn, StateColumn, EntriesColumn, ColumnCount };
const int RootRole = Qt::UserRole + 1;

} // namespace

QString indexStateText(const index::RootStatus &status)
{
    using State = index::RootStatus::State;
    const QString folders = QLocale().toString(status.progress.foldersScanned);
    switch (status.state) {
    case State::Building:
        if (status.expectedEntries > 0) {
            // La tabla de archivos de la unidad dice cuántas entradas esperar.
            const qint64 percent = std::min<qint64>(99, status.progress.entriesSeen * 100 / status.expectedEntries);
            return IndexDialog::tr("Indexando… %1 % (%2 carpetas)").arg(percent).arg(folders);
        }
        return IndexDialog::tr("Indexando… %1 carpetas").arg(folders);
    case State::Updating:
        return IndexDialog::tr("Poniéndose al día… %1 carpetas").arg(folders);
    case State::Disconnected:
        return IndexDialog::tr("Unidad no conectada (se conserva el índice)");
    case State::Failed:
        return IndexDialog::tr("Error: %1").arg(status.error);
    case State::Ready:
        break;
    }
    if (status.watching && !status.watchLimited) {
        return IndexDialog::tr("Al día (sigue los cambios)");
    }
    return status.watchLimited ? IndexDialog::tr("Al día (vigilancia parcial: se repasa cada 30 min)")
                               : IndexDialog::tr("Al día (se repasa cada 30 min)");
}

IndexDialog::IndexDialog(index::IndexService &service, VolumeSource volumes, QWidget *parent)
    : QDialog(parent), m_service(service), m_volumes(std::move(volumes))
{
    setWindowTitle(tr("Índice de búsqueda"));
    resize(760, 420);

    auto *intro = new QLabel(tr("Con un índice, buscar por nombre en una unidad entera es casi instantáneo. "
                                "Marca las unidades que quieras indexar. El índice se crea en segundo plano, "
                                "anota nombres, tamaños y fechas (no el contenido de los archivos) y se guarda "
                                "solo en este equipo."), this);
    intro->setWordWrap(true);

    m_list = new QTreeWidget(this);
    m_list->setColumnCount(ColumnCount);
    m_list->setHeaderLabels({tr("Unidad"), tr("Ubicación"), tr("Estado"), tr("Entradas")});
    m_list->setRootIsDecorated(false);
    m_list->setUniformRowHeights(true);
    m_list->setAccessibleName(tr("Unidades indexadas"));
    m_list->header()->resizeSection(NameColumn, 170);
    m_list->header()->resizeSection(LocationColumn, 190);
    m_list->header()->setSectionResizeMode(StateColumn, QHeaderView::Stretch);
    m_list->header()->setStretchLastSection(false);
    m_list->header()->resizeSection(EntriesColumn, 90);

    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    m_note->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_note->hide();

    m_refreshButton = new QPushButton(tr("Actualizar ahora"), this);
    m_refreshButton->setToolTip(tr("Vuelve a recorrer la unidad seleccionada (o todas) para poner su índice al día"));
    m_rebuildButton = new QPushButton(tr("Reconstruir"), this);
    m_rebuildButton->setToolTip(tr("Borra el índice de la unidad seleccionada y lo crea de nuevo"));
    m_pauseButton = new QPushButton(this);
    auto *closeButton = new QPushButton(tr("Cerrar"), this);
    closeButton->setDefault(true);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_refreshButton);
    buttons->addWidget(m_rebuildButton);
    buttons->addWidget(m_pauseButton);
    buttons->addStretch(1);
    buttons->addWidget(closeButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(intro);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_note);

    // El servicio opcional de Windows (SPEC §6.4): solo con consentimiento
    // explícito y con el permiso de administrador que pide Windows.
    auto *serviceBox = new QGroupBox(tr("Búsqueda acelerada en Windows (opcional)"), this);
    m_serviceBox = serviceBox;
    m_serviceLabel = new QLabel(serviceBox);
    m_serviceLabel->setWordWrap(true);
    m_serviceButton = new QPushButton(serviceBox);
    auto *serviceLayout = new QHBoxLayout(serviceBox);
    serviceLayout->addWidget(m_serviceLabel, 1);
    serviceLayout->addWidget(m_serviceButton, 0, Qt::AlignTop);
    layout->addWidget(serviceBox);
    connect(m_serviceButton, &QPushButton::clicked, this, [this] {
        runServiceSetup(index::journalservice::state() == index::journalservice::State::NotInstalled);
    });
    m_setupTimer.setInterval(300);
    connect(&m_setupTimer, &QTimer::timeout, this, [this] {
        if (m_setup && m_setup->isFinished()) {
            onServiceSetupFinished();
        }
    });
    updateService();

    layout->addLayout(buttons);

    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_pauseButton, &QPushButton::clicked, this, [this] { m_service.setPaused(!m_service.isPaused()); });
    connect(m_refreshButton, &QPushButton::clicked, this, [this] {
        const QString selected = selectedRoot();
        const QStringList roots = selected.isEmpty() ? m_service.roots() : QStringList{selected};
        for (const QString &root : roots) {
            m_service.refresh(root);
        }
    });
    connect(m_rebuildButton, &QPushButton::clicked, this, [this] {
        const QString selected = selectedRoot();
        if (!selected.isEmpty()) {
            m_service.rebuild(selected);
        }
    });
    connect(m_list, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
        if (m_updating || column != NameColumn) {
            return;
        }
        const QString root = item->data(NameColumn, RootRole).toString();
        const bool wanted = item->checkState(NameColumn) == Qt::Checked;
        const bool indexed = m_service.roots().contains(root);
        if (wanted && !indexed) {
            m_service.addRoot(root);
            emit rootsChanged();
        } else if (!wanted && indexed) {
            m_service.removeRoot(root);
            emit rootsChanged();
        }
    });
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, &IndexDialog::updateRows);
    connect(&m_service, &index::IndexService::changed, this, &IndexDialog::updateRows);

    rebuild();
}

void IndexDialog::rebuild()
{
    m_updating = true;
    m_list->clear();
    QStringList listed;
    const auto addRow = [&](const QString &root, const QString &name, const QIcon &icon) {
        auto *item = new QTreeWidgetItem(m_list);
        item->setData(NameColumn, RootRole, root);
        item->setText(NameColumn, name);
        item->setIcon(NameColumn, icon);
        item->setText(LocationColumn, QDir::toNativeSeparators(root));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setTextAlignment(EntriesColumn, Qt::AlignRight | Qt::AlignVCenter);
        listed.append(root);
    };
    for (const fs::Volume &volume : m_volumes()) {
        addRow(QDir::cleanPath(volume.rootPath), volume.displayName, iconForVolume(volume));
    }
    // Lo que se indexa aunque ahora no aparezca como unidad (está desconectada).
    for (const QString &root : m_service.roots()) {
        if (!listed.contains(root)) {
            addRow(root, QDir(root).dirName(), QIcon());
        }
    }
    m_updating = false;
    updateRows();
}

void IndexDialog::updateRows()
{
    m_updating = true;
    bool limited = false;
    for (int row = 0; row < m_list->topLevelItemCount(); ++row) {
        QTreeWidgetItem *item = m_list->topLevelItem(row);
        const auto status = m_service.statusOf(item->data(NameColumn, RootRole).toString());
        item->setCheckState(NameColumn, status ? Qt::Checked : Qt::Unchecked);
        item->setText(StateColumn, status ? indexStateText(*status) : tr("Sin índice"));
        item->setText(EntriesColumn, (status && status->info.entries > 0)
                                         ? QLocale().toString(status->info.entries) : QString());
        if (status && status->info.updatedMs > 0) {
            QString tip = tr("Último recorrido completo: %1")
                              .arg(QLocale().toString(QDateTime::fromMSecsSinceEpoch(status->info.updatedMs),
                                                      QLocale::ShortFormat));
            if (status->journalFolders >= 0) {
                tip += QLatin1Char('\n')
                    + tr("Al abrir se puso al día con el diario de cambios de NTFS: %n carpeta(s) cambiada(s).",
                         nullptr, int(std::min<qint64>(status->journalFolders, INT_MAX)));
            }
            item->setToolTip(StateColumn, tip);
        }
        limited = limited || (status && status->watchLimited);
    }
    m_updating = false;

    m_pauseButton->setText(m_service.isPaused() ? tr("Reanudar la indexación") : tr("Pausar la indexación"));
    m_pauseButton->setEnabled(!m_service.roots().isEmpty());
    m_refreshButton->setEnabled(!m_service.roots().isEmpty());
    m_rebuildButton->setEnabled(m_service.roots().contains(selectedRoot()));
    if (limited) {
        // SPEC §6.4: explicar el límite de inotify y cómo ampliarlo.
        m_note->setText(tr("Linux limita cuántas carpetas puede vigilar cada usuario, y aquí hay más de las que "
                           "caben: las que no se vigilan se repasan cada 30 minutos, o al pulsar «Actualizar "
                           "ahora». Para vigilarlas todas, amplía el límite en una terminal:\n"
                           "    sudo sysctl fs.inotify.max_user_watches=1048576\n"
                           "y, para que se conserve al reiniciar:\n"
                           "    echo fs.inotify.max_user_watches=1048576 | sudo tee /etc/sysctl.d/60-inotify.conf\n"
                           "Después reinicia Explorador Ultra."));
    }
    m_note->setVisible(limited);
}

void IndexDialog::updateService()
{
    using State = index::journalservice::State;
    const State state = index::journalservice::state();
    m_serviceBox->setVisible(state != State::Unavailable);
    if (state == State::Unavailable) {
        return;
    }
    if (m_setup) {
        m_serviceButton->setEnabled(false);
        m_serviceLabel->setText(m_setupInstalls ? tr("Instalando el servicio…") : tr("Quitando el servicio…"));
        return;
    }
    m_serviceButton->setEnabled(true);
    QString text;
    switch (state) {
    case State::NotInstalled:
        text = tr("Un servicio de Explorador Ultra puede leer el diario de cambios de NTFS para que, al abrir la "
                  "aplicación, los índices se pongan al día en segundos en vez de recorrer las unidades enteras. "
                  "Solo lee, no envía nada fuera del equipo y se puede quitar cuando quieras; instalarlo pide "
                  "permiso de administrador. Sin él todo funciona igual.");
        m_serviceButton->setText(tr("Instalar…"));
        break;
    case State::Running:
        text = tr("El servicio está instalado y en marcha: los índices de las unidades NTFS se ponen al día con "
                  "su diario de cambios.");
        m_serviceButton->setText(tr("Desinstalar…"));
        break;
    case State::Stopped:
        text = tr("El servicio está instalado pero detenido: Windows lo arranca al iniciar. Mientras tanto, los "
                  "índices se ponen al día recorriendo las unidades.");
        m_serviceButton->setText(tr("Desinstalar…"));
        break;
    case State::Unavailable:
        break;
    }
    if (!m_serviceMessage.isEmpty()) {
        text = m_serviceMessage + QLatin1Char(' ') + text;
    }
    m_serviceLabel->setText(text);
}

void IndexDialog::runServiceSetup(bool install)
{
    const QString question =
        install ? tr("Se instalará el servicio «Explorador Ultra: índice de NTFS», que se ejecuta con permisos de "
                     "sistema y solo lee el diario de cambios de las unidades NTFS. Windows pedirá permiso de "
                     "administrador. ¿Instalarlo?")
                : tr("Se parará y se quitará el servicio. Windows pedirá permiso de administrador. Los índices se "
                     "pondrán al día recorriendo las unidades, como antes. ¿Quitarlo?");
    if (QMessageBox::question(this, install ? tr("Instalar el servicio") : tr("Quitar el servicio"), question)
        != QMessageBox::Yes) {
        return;
    }
    m_serviceMessage.clear();
    m_serviceButton->setEnabled(false);
    m_serviceLabel->setText(tr("Esperando el permiso de administrador…"));
    const core::Result<index::journalservice::Setup> started = index::journalservice::Setup::start(install);
    if (!started.ok()) {
        m_serviceMessage = started.error().code == core::ErrorCode::Cancelled
            ? tr("No se dio el permiso: no se cambió nada.")
            : tr("No se pudo: %1.").arg(started.error().message);
        updateService();
        return;
    }
    m_setup = started.value();
    m_setupInstalls = install;
    m_setupTimer.start();
    updateService();
}

void IndexDialog::onServiceSetupFinished()
{
    m_setupTimer.stop();
    const int code = m_setup->exitCode();
    m_setup.reset();
    // Los códigos de salida de eu-indexsvc (src/indexsvc/main.cpp).
    switch (code) {
    case 0:
        m_serviceMessage = m_setupInstalls ? tr("Instalado.") : tr("Quitado.");
        break;
    case 2:
        m_serviceMessage = tr("No se pudo: hace falta permiso de administrador.");
        break;
    case 3:
        m_serviceMessage = tr("No se pudo copiar el servicio a «Archivos de programa».");
        break;
    case 4:
        m_serviceMessage = tr("El administrador de servicios de Windows no lo permitió.");
        break;
    case 5:
        m_serviceMessage = tr("Se instaló pero no arrancó; el motivo está en el Visor de eventos de Windows.");
        break;
    default:
        m_serviceMessage = tr("No se pudo (código %1).").arg(code);
        break;
    }
    updateService();
}

QTreeWidgetItem *IndexDialog::itemFor(const QString &root) const
{
    for (int row = 0; row < m_list->topLevelItemCount(); ++row) {
        if (m_list->topLevelItem(row)->data(NameColumn, RootRole).toString() == root) {
            return m_list->topLevelItem(row);
        }
    }
    return nullptr;
}

QString IndexDialog::selectedRoot() const
{
    const QTreeWidgetItem *item = m_list->currentItem();
    return (item != nullptr && item->isSelected()) ? item->data(NameColumn, RootRole).toString() : QString();
}

void IndexDialog::setIndexed(const QString &root, bool indexed)
{
    if (QTreeWidgetItem *item = itemFor(QDir::cleanPath(root))) {
        item->setCheckState(NameColumn, indexed ? Qt::Checked : Qt::Unchecked);
    }
}

} // namespace eu::ui
