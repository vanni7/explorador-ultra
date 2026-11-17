// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/TagsDialog.h"

#include <QApplication>
#include <QColorDialog>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace eu::ui {

namespace {
const int IdRole = Qt::UserRole + 1;
}

QIcon tagIcon(const QString &color)
{
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(color));
    painter.drawEllipse(QRectF(2, 2, 12, 12));
    return QIcon(pixmap);
}

TagsDialog::TagsDialog(tasks::Scheduler &scheduler, std::shared_ptr<tags::TagStore> store, QStringList searchRoots,
                       QWidget *parent)
    : QDialog(parent), m_scheduler(scheduler), m_store(std::move(store)), m_searchRoots(std::move(searchRoots))
{
    setWindowTitle(tr("Etiquetas"));
    resize(720, 480);
    auto *tabs = new QTabWidget(this);

    // Etiquetas.
    auto *tagsPage = new QWidget(tabs);
    m_tags = new QListWidget(tagsPage);
    auto *newButton = new QPushButton(tr("Nueva…"), tagsPage);
    auto *renameButton = new QPushButton(tr("Renombrar…"), tagsPage);
    auto *colorButton = new QPushButton(tr("Color…"), tagsPage);
    auto *deleteButton = new QPushButton(tr("Borrar"), tagsPage);
    auto *tagButtons = new QVBoxLayout;
    for (QPushButton *button : {newButton, renameButton, colorButton, deleteButton}) {
        tagButtons->addWidget(button);
    }
    tagButtons->addStretch(1);
    auto *tagsLayout = new QHBoxLayout(tagsPage);
    tagsLayout->addWidget(m_tags, 1);
    tagsLayout->addLayout(tagButtons);
    tabs->addTab(tagsPage, tr("Etiquetas"));

    // Huérfanas.
    auto *orphansPage = new QWidget(tabs);
    m_orphanNote = new QLabel(tr("Una etiqueta queda huérfana si su archivo se movió o se borró fuera de la aplicación y "
                                 "no se puede saber sin dudas dónde está. Nunca se le pasa a otro archivo por "
                                 "parecido: puedes buscarla de nuevo (solo se reasigna si hay un único candidato) o "
                                 "quitarla."),
                              orphansPage);
    m_orphanNote->setWordWrap(true);
    m_orphans = new QTreeWidget(orphansPage);
    m_orphans->setColumnCount(2);
    m_orphans->setHeaderLabels({tr("Etiqueta"), tr("Dónde estaba")});
    m_orphans->setRootIsDecorated(false);
    m_orphans->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_orphans->header()->resizeSection(0, 140);
    auto *reviewButton = new QPushButton(tr("Repasar ahora"), orphansPage);
    auto *relocateButton = new QPushButton(tr("Buscar de nuevo"), orphansPage);
    auto *removeButton = new QPushButton(tr("Quitar"), orphansPage);
    auto *orphanButtons = new QHBoxLayout;
    orphanButtons->addWidget(reviewButton);
    orphanButtons->addWidget(relocateButton);
    orphanButtons->addWidget(removeButton);
    orphanButtons->addStretch(1);
    auto *orphansLayout = new QVBoxLayout(orphansPage);
    orphansLayout->addWidget(m_orphanNote);
    orphansLayout->addWidget(m_orphans, 1);
    orphansLayout->addLayout(orphanButtons);
    tabs->addTab(orphansPage, tr("Huérfanas"));

    auto *closeButton = new QPushButton(tr("Cerrar"), this);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs, 1);
    layout->addWidget(closeButton, 0, Qt::AlignRight);

    const auto selectedId = [this]() -> qint64 {
        const QListWidgetItem *item = m_tags->currentItem();
        return item != nullptr ? item->data(IdRole).toLongLong() : 0;
    };
    connect(newButton, &QPushButton::clicked, this, [this] {
        const QString name = QInputDialog::getText(this, tr("Nueva etiqueta"), tr("Nombre:"));
        if (name.trimmed().isEmpty()) {
            return;
        }
        const QStringList colors = tags::defaultColors();
        const auto created = m_store->createTag(name, colors.value(int(m_store->tags().size()) % colors.size()));
        if (!created.ok()) {
            QMessageBox::warning(this, windowTitle(), created.error().message);
        }
        reloadTags();
        emit tagsChanged();
    });
    connect(renameButton, &QPushButton::clicked, this, [this, selectedId] {
        const qint64 id = selectedId();
        if (id == 0) {
            return;
        }
        const QString name = QInputDialog::getText(this, tr("Renombrar etiqueta"), tr("Nombre:"), QLineEdit::Normal,
                                                   m_tags->currentItem()->text());
        if (!name.trimmed().isEmpty()) {
            const core::Status renamed = m_store->renameTag(id, name);
            if (!renamed.ok()) {
                QMessageBox::warning(this, windowTitle(), renamed.error().message);
            }
            reloadTags();
            emit tagsChanged();
        }
    });
    connect(colorButton, &QPushButton::clicked, this, [this, selectedId] {
        const qint64 id = selectedId();
        if (id == 0) {
            return;
        }
        const QColor color = QColorDialog::getColor(QColor(m_tags->currentItem()->data(Qt::UserRole + 2).toString()), this);
        if (color.isValid()) {
            m_store->setColor(id, color.name());
            reloadTags();
            emit tagsChanged();
        }
    });
    connect(deleteButton, &QPushButton::clicked, this, [this, selectedId] {
        const qint64 id = selectedId();
        if (id == 0) {
            return;
        }
        if (QMessageBox::question(this, windowTitle(),
                                  tr("¿Borrar la etiqueta «%1»? Se quita de todos los archivos (los archivos no se tocan).")
                                      .arg(m_tags->currentItem()->text()))
            == QMessageBox::Yes) {
            m_store->deleteTag(id);
            reloadTags();
            emit tagsChanged();
        }
    });
    connect(reviewButton, &QPushButton::clicked, this, &TagsDialog::reviewOrphans);
    connect(relocateButton, &QPushButton::clicked, this, &TagsDialog::relocateSelected);
    connect(removeButton, &QPushButton::clicked, this, [this] {
        for (QTreeWidgetItem *item : m_orphans->selectedItems()) {
            m_store->removeMark(item->data(0, IdRole).toLongLong());
        }
        reloadOrphans(m_store->orphans());
    });
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);
    reloadTags();
    reloadOrphans(m_store->orphans());
}

TagsDialog::~TagsDialog()
{
    m_task.cancel();
}

void TagsDialog::reloadTags()
{
    m_tags->clear();
    for (const tags::Tag &tag : m_store->tags()) {
        auto *item = new QListWidgetItem(tagIcon(tag.color), tag.name, m_tags);
        item->setData(IdRole, tag.id);
        item->setData(Qt::UserRole + 2, tag.color);
    }
}

void TagsDialog::reloadOrphans(const QList<tags::Orphan> &orphans)
{
    m_orphans->clear();
    for (const tags::Orphan &orphan : orphans) {
        auto *item = new QTreeWidgetItem(m_orphans);
        item->setIcon(0, tagIcon(orphan.tag.color));
        item->setText(0, orphan.tag.name);
        item->setText(1, QDir::toNativeSeparators(orphan.path));
        item->setData(0, IdRole, orphan.markId);
    }
}

void TagsDialog::reviewOrphans()
{
    m_busy = true;
    m_orphanNote->setText(tr("Repasando…"));
    const QPointer<TagsDialog> self(this);
    m_task = m_scheduler.submit(tasks::Category::Metadata, tasks::Priority::UserInteraction,
                                [self, store = m_store](const tasks::CancelToken &) {
        const QList<tags::Orphan> orphans = store->review();
        QMetaObject::invokeMethod(qApp, [self, orphans] {
            if (self) {
                self->m_busy = false;
                self->m_orphanNote->setText(orphans.isEmpty() ? tr("Ninguna huérfana: todas las etiquetas encuentran su archivo.")
                                                              : tr("%n huérfana(s).", nullptr, int(orphans.size())));
                self->reloadOrphans(orphans);
            }
        }, Qt::QueuedConnection);
    });
}

void TagsDialog::relocateSelected()
{
    QList<qint64> marks;
    QStringList roots;
    for (QTreeWidgetItem *item : m_orphans->selectedItems()) {
        marks.append(item->data(0, IdRole).toLongLong());
        // Se busca en la unidad donde estaba (si sigue conectada).
        const QString old = QDir::fromNativeSeparators(item->text(1));
        for (const QString &root : std::as_const(m_searchRoots)) {
            if (old.startsWith(root) && !roots.contains(root)) {
                roots.append(root);
            }
        }
    }
    if (marks.isEmpty()) {
        return;
    }
    if (roots.isEmpty()) {
        roots = m_searchRoots;
    }
    m_busy = true;
    m_orphanNote->setText(tr("Buscando…"));
    const QPointer<TagsDialog> self(this);
    m_task = m_scheduler.submitForPaths(tasks::Category::Indexing, tasks::Priority::UserInteraction, roots,
                                        [self, store = m_store, marks, roots](const tasks::CancelToken &cancel) {
        int found = 0;
        for (const qint64 mark : marks) {
            const core::Result<bool> relocated = store->relocate(mark, roots, cancel);
            found += relocated.ok() && relocated.value() ? 1 : 0;
        }
        const QList<tags::Orphan> orphans = store->orphans();
        const int asked = int(marks.size());
        QMetaObject::invokeMethod(qApp, [self, orphans, found, asked] {
            if (self) {
                self->m_busy = false;
                self->m_orphanNote->setText(tr("Encontrado(s) %1 de %2. Lo que tiene varios candidatos o ninguno sigue "
                                               "huérfano.").arg(found).arg(asked));
                self->reloadOrphans(orphans);
                emit self->tagsChanged();
            }
        }, Qt::QueuedConnection);
    });
}

} // namespace eu::ui
