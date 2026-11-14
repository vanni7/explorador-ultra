// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/index/TreeWatcher.h"

#include <QDir>

namespace eu::index {

TreeWatcher::TreeWatcher(QString root) : m_root(QDir::cleanPath(root)) {}

TreeWatcher::~TreeWatcher() = default;

#if !defined(Q_OS_LINUX) && !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
namespace {

/// Sistemas sin vigilancia propia: el índice se pone al día al arrancar y a mano.
class NullTreeWatcher : public TreeWatcher
{
public:
    using TreeWatcher::TreeWatcher;
    bool isActive() const override { return false; }
    void addFolder(const QString &) override {}
};

} // namespace

std::unique_ptr<TreeWatcher> TreeWatcher::create(const QString &root)
{
    return std::unique_ptr<TreeWatcher>(new NullTreeWatcher(root));
}
#endif

} // namespace eu::index
