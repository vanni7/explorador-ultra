// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/BrowserPane.h"
#include "eu/core/SizeFormat.h"
#include "eu/ui/FileClipboard.h"
#include "eu/ui/Locations.h"
#include "eu/ui/MainWindow.h"
#include "eu/ui/MillerView.h"
#include "eu/ui/SyncDialog.h"
#include "eu/ui/Appearance.h"
#include "eu/ui/PreferencesDialog.h"
#include "eu/ui/Printing.h"
#include "eu/ui/Shortcuts.h"
#include "eu/ui/BulkRenameDialog.h"
#include "eu/ui/DuplicatesDialog.h"
#include "eu/ui/SimilarImagesDialog.h"
#include "eu/ui/OrganizeDialog.h"
#include "eu/ui/PathBar.h"
#include "eu/ui/SearchResultsModel.h"
#include "eu/ui/IndexDialog.h"
#include "eu/archive/Archive.h"
#include "eu/ui/InlinePlayer.h"
#include "eu/ui/MediaPreview.h"
#include "../support/TestMedia.h"
#include "eu/ui/PreviewPanel.h"
#include "eu/ui/Sidebar.h"
#include "eu/ui/ThumbnailView.h"
#include "eu/ui/TrashDialog.h"
#include "eu/ui/VolumesView.h"

#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLineEdit>
#include <QMimeData>
#include <QProcess>
#include <QItemSelectionModel>
#include <QLabel>
#include <QSettings>
#include <QTimer>
#include <QMenu>
#include <QImageReader>
#include <QPdfWriter>
#include <QPrinter>
#include <QStyle>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QPainter>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeView>
#include <QUrl>
#include <QWheelEvent>

#include <cstdlib>
#include <memory>

using namespace eu;
using namespace eu::ui;

namespace {

void writeFile(const QString &path, qsizetype bytes = 0)
{
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(path));
    file.write(QByteArray(bytes, 'x'));
}

bool waitReady(BrowserPane *pane)
{
    return QTest::qWaitFor([pane] { return pane->model()->state() != DirectoryModel::State::Loading; }, 20000);
}

/// Espera a que la ventana no tenga operaciones de archivos en curso.
bool waitIdle(MainWindow &window)
{
    return QTest::qWaitFor([&window] { return !window.fileActions()->isBusy(); }, 30000);
}

void selectNames(BrowserPane *pane, const QStringList &names)
{
    pane->view()->selectionModel()->clearSelection();
    for (const QString &name : names) {
        const int row = pane->model()->rowOfName(name);
        QVERIFY2(row >= 0, qPrintable(name));
        pane->view()->selectionModel()->select(pane->model()->index(row, 0),
                                               QItemSelectionModel::Select | QItemSelectionModel::Rows);
        // Solo mueve el cursor: setCurrentIndex de la vista deseleccionaría lo anterior.
        pane->view()->selectionModel()->setCurrentIndex(pane->model()->index(row, 0), QItemSelectionModel::NoUpdate);
    }
}

std::shared_ptr<journal::Journal> openJournal(const QTemporaryDir &directory)
{
    auto journal = journal::Journal::open(directory.filePath(QStringLiteral("historial.db")));
    return journal.ok() ? journal.value() : nullptr;
}

} // namespace

class TestMainWindow : public QObject
{
    Q_OBJECT

private:
    tasks::Scheduler m_scheduler;

private slots:
    void initTestCase()
    {
        // Las pruebas no deben leer ni escribir la configuración real del usuario.
        QStandardPaths::setTestModeEnabled(true);
        // Igual que en main(): sin estos nombres QSettings no tiene dónde guardar en Windows.
        QCoreApplication::setOrganizationName(QStringLiteral("ExploradorUltraPruebas"));
        QCoreApplication::setApplicationName(QStringLiteral("test_ui_main_window"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings().clear();
    }

    void parentPaths()
    {
        QCOMPARE(parentPathOf(QStringLiteral("/home/ana/Fotos")), QStringLiteral("/home/ana"));
        QCOMPARE(parentPathOf(QStringLiteral("/home")), QStringLiteral("/"));
        QCOMPARE(parentPathOf(QStringLiteral("/")), QString());
        QCOMPARE(parentPathOf(QStringLiteral("/home/ana/")), QStringLiteral("/home"));
        QCOMPARE(parentPathOf(QString()), QString());
#ifdef Q_OS_WIN
        QCOMPARE(parentPathOf(QStringLiteral("C:/Users")), QStringLiteral("C:/"));
        QCOMPARE(parentPathOf(QStringLiteral("C:/")), QString());
#endif
    }

    void crumbs()
    {
        const auto unixCrumbs = PathBar::crumbsFor(QStringLiteral("/home/ana/Fotos 2026"));
        QCOMPARE(unixCrumbs.size(), 4);
        QCOMPARE(unixCrumbs[0].second, QStringLiteral("/"));
        QCOMPARE(unixCrumbs[1].first, QStringLiteral("home"));
        QCOMPARE(unixCrumbs[3].first, QStringLiteral("Fotos 2026"));
        QCOMPARE(unixCrumbs[3].second, QStringLiteral("/home/ana/Fotos 2026"));

        const auto windowsCrumbs = PathBar::crumbsFor(QStringLiteral("C:/Users/ana"));
        QCOMPARE(windowsCrumbs.size(), 3);
        QCOMPARE(windowsCrumbs[0].first, QStringLiteral("C:"));
        QCOMPARE(windowsCrumbs[0].second, QStringLiteral("C:/"));
        QCOMPARE(windowsCrumbs[2].second, QStringLiteral("C:/Users/ana"));

        QVERIFY(PathBar::crumbsFor(QString()).isEmpty());
    }

    void navigationAndHistory()
    {
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("a/b")));
        writeFile(directory.filePath(QStringLiteral("a/archivo.txt")), 2000);

        MainWindow window(m_scheduler);
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        QCOMPARE(pane->currentPath(), QDir::cleanPath(directory.path()));
        QVERIFY(!pane->canGoBack());
        QVERIFY(pane->canGoUp());

        // Entrar en "a" activando su fila, como con doble clic o Intro.
        const int rowA = pane->model()->rowOfName(QStringLiteral("a"));
        QVERIFY(rowA >= 0);
        emit pane->view()->activated(pane->model()->index(rowA, 0));
        QVERIFY(waitReady(pane));
        QCOMPARE(pane->currentPath(), directory.filePath(QStringLiteral("a")));
        QVERIFY(pane->canGoBack());
        QCOMPARE(pane->model()->rowCount(), 2);

        // Seleccionar el archivo: la barra de estado dice cuánto ocupa.
        const int fileRow = pane->model()->rowOfName(QStringLiteral("archivo.txt"));
        pane->view()->selectionModel()->select(pane->model()->index(fileRow, 0),
                                               QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QVERIFY(pane->statusText().contains(QStringLiteral("2")));
        QCOMPARE(pane->selectedPaths(), QStringList{directory.filePath(QStringLiteral("a/archivo.txt"))});

        pane->navigateTo(directory.filePath(QStringLiteral("a/b")));
        QVERIFY(waitReady(pane));

        // Subir deja seleccionada la carpeta de la que se viene.
        pane->goUp();
        QVERIFY(waitReady(pane));
        QCOMPARE(pane->currentPath(), directory.filePath(QStringLiteral("a")));
        QTRY_COMPARE(pane->selectedPaths(), QStringList{directory.filePath(QStringLiteral("a/b"))});

        pane->goBack();
        QVERIFY(waitReady(pane));
        QCOMPARE(pane->currentPath(), directory.filePath(QStringLiteral("a/b")));
        QVERIFY(pane->canGoForward());
        pane->goForward();
        QVERIFY(waitReady(pane));
        QCOMPARE(pane->currentPath(), directory.filePath(QStringLiteral("a")));
        QVERIFY(!pane->canGoForward());
    }

    void viewUpdatesWhenFolderChangesOnDisk()
    {
        QTemporaryDir directory;
        MainWindow window(m_scheduler);
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        QCOMPARE(pane->model()->rowCount(), 0);

        writeFile(directory.filePath(QStringLiteral("nuevo.txt")));
        QTRY_COMPARE_WITH_TIMEOUT(pane->model()->rowCount(), 1, 8000);

        QVERIFY(QFile::remove(directory.filePath(QStringLiteral("nuevo.txt"))));
        QTRY_COMPARE_WITH_TIMEOUT(pane->model()->rowCount(), 0, 8000);
    }

    void missingFolderShowsErrorWithoutBlocking()
    {
        QTemporaryDir directory;
        MainWindow window(m_scheduler);
        BrowserPane *pane = window.openTab(directory.filePath(QStringLiteral("no-existe")));
        QVERIFY(waitReady(pane));
        QCOMPARE(pane->model()->state(), DirectoryModel::State::Failed);
        QVERIFY(!pane->statusText().isEmpty());

        // Tras crear la carpeta, "Reintentar" la muestra.
        QVERIFY(QDir().mkpath(directory.filePath(QStringLiteral("no-existe"))));
        pane->refresh();
        QVERIFY(waitReady(pane));
        QCOMPARE(pane->model()->state(), DirectoryModel::State::Ready);
    }

    void tabsOpenAndClose()
    {
        QTemporaryDir first;
        QTemporaryDir second;
        MainWindow window(m_scheduler);
        window.openTab(first.path());
        BrowserPane *other = window.openTab(second.path());
        QCOMPARE(window.tabCount(), 2);
        QCOMPARE(window.currentPane(), other);
        QCOMPARE(window.currentPane()->currentPath(), QDir::cleanPath(second.path()));
        QVERIFY(waitReady(other));

        // La ✕ de una pestaña con una búsqueda sale de la búsqueda, no cierra
        // nada (con una sola pestaña, cerraba la ventana entera).
        window.closeTab(0);
        QCOMPARE(window.tabCount(), 1);
        window.searchEdit()->setText(QStringLiteral("*"));
        QTest::keyClick(window.searchEdit(), Qt::Key_Return);
        QVERIFY(other->showsSearch());
        window.closeTab(0);
        QCOMPARE(window.tabCount(), 1);
        QVERIFY(!other->showsSearch());
        QCOMPARE(other->currentPath(), QDir::cleanPath(second.path()));
    }

    void fileClipboardRoundTrip()
    {
        const QStringList paths{QStringLiteral("/tmp/a b.txt"), QStringLiteral("/tmp/ñandú")};
        std::unique_ptr<QMimeData> copied(makeFileMimeData(paths, false));
        QCOMPARE(fileSelectionFrom(copied.get()).paths, paths);
        QVERIFY(!fileSelectionFrom(copied.get()).cut);
        std::unique_ptr<QMimeData> cut(makeFileMimeData(paths, true));
        QVERIFY(fileSelectionFrom(cut.get()).cut);

        // Lo que pone Nautilus al cortar: solo su formato propio y las URL.
        QMimeData fromGnome;
        fromGnome.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/x"))});
        fromGnome.setData(QStringLiteral("x-special/gnome-copied-files"), "cut\nfile:///tmp/x");
        QVERIFY(fileSelectionFrom(&fromGnome).cut);

        // Texto corriente o direcciones web no son archivos.
        QMimeData text;
        text.setText(QStringLiteral("hola"));
        QVERIFY(fileSelectionFrom(&text).isEmpty());
        QMimeData web;
        web.setUrls({QUrl(QStringLiteral("https://example.org/a"))});
        QVERIFY(fileSelectionFrom(&web).isEmpty());
    }

    void newFolderIsCreatedSelectedAndReadyToName()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("Nueva carpeta")));
        MainWindow window(m_scheduler, openJournal(data));
        window.show();
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));

        window.newFolder();
        // Ya existía "Nueva carpeta": la nueva recibe otro nombre.
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(directory.filePath(QStringLiteral("Nueva carpeta (2)"))).isDir(), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(pane->selectedPaths(),
                                  QStringList{directory.filePath(QStringLiteral("Nueva carpeta (2)"))}, 10000);
        // Queda abierta la casilla para escribirle el nombre definitivo.
        QTRY_VERIFY_WITH_TIMEOUT(pane->view()->findChild<QLineEdit *>() != nullptr, 5000);
        auto *editor = pane->view()->findChild<QLineEdit *>();
        QCOMPARE(editor->text(), QStringLiteral("Nueva carpeta (2)"));

        editor->setText(QStringLiteral("Viajes"));
        QTest::keyClick(editor, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo(directory.filePath(QStringLiteral("Viajes"))).isDir(), 10000);
        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("Nueva carpeta (2)"))));
        QTRY_COMPARE_WITH_TIMEOUT(pane->selectedPaths(), QStringList{directory.filePath(QStringLiteral("Viajes"))},
                                  10000);
        QVERIFY(waitIdle(window));
    }

    void renameKeepsExtensionOutOfTheSelection()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("informe.pdf")));
        MainWindow window(m_scheduler, openJournal(data));
        window.show();
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        selectNames(pane, {QStringLiteral("informe.pdf")});

        window.renameSelection();
        QTRY_VERIFY(pane->view()->findChild<QLineEdit *>() != nullptr);
        auto *editor = pane->view()->findChild<QLineEdit *>();
        QTRY_COMPARE(editor->selectedText(), QStringLiteral("informe"));

        // Un nombre no válido se rechaza con un aviso y el archivo no cambia.
        QSignalSpy reports(&window, &MainWindow::actionReported);
        editor->setText(QStringLiteral("a/b.pdf"));
        QTest::keyClick(editor, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(reports.count(), 1, 10000);
        QCOMPARE(reports.first().at(1).toBool(), true);
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("informe.pdf"))));
        QVERIFY(waitIdle(window));
    }

    void copyCutPasteAndUndo()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("origen")));
        writeFile(directory.filePath(QStringLiteral("origen/foto.jpg")), 1500);
        writeFile(directory.filePath(QStringLiteral("origen/nota.txt")), 10);
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("destino")));
        const QString source = directory.filePath(QStringLiteral("origen"));
        const QString destination = directory.filePath(QStringLiteral("destino"));

        MainWindow window(m_scheduler, openJournal(data));
        window.show();
        BrowserPane *pane = window.openTab(source);
        QVERIFY(waitReady(pane));

        // Copiar y pegar en otra carpeta.
        selectNames(pane, {QStringLiteral("foto.jpg"), QStringLiteral("nota.txt")});
        window.copySelection();
        pane->navigateTo(destination);
        QVERIFY(waitReady(pane));
        window.paste();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(destination + QStringLiteral("/foto.jpg")), 10000);
        QVERIFY(waitIdle(window));
        QVERIFY(QFileInfo::exists(source + QStringLiteral("/foto.jpg")));
        // Lo pegado queda seleccionado.
        QTRY_COMPARE_WITH_TIMEOUT(pane->selectedPaths().size(), 2, 10000);

        // Pegar otra vez donde ya están: se pregunta. Con «mantener los dos»
        // y «lo mismo con los demás», una sola pregunta y copias con otro nombre.
        int asked = 0;
        window.setCollisionAnswerForTesting([&](const ops::Conflict &conflict, const tasks::CancelToken &) {
            ++asked;
            [&] { QVERIFY(conflict.target.startsWith(destination)); }();
            return ops::CollisionDecision{ops::CollisionPolicy::AutoRename, true, false};
        });
        window.paste();
        QTRY_COMPARE_WITH_TIMEOUT(QDir(destination).entryList(QDir::Files).size(), 4, 10000);
        QVERIFY(waitIdle(window));
        QCOMPARE(asked, 1);

        // Deshacer quita solo esas últimas copias.
        window.undo();
        QTRY_COMPARE_WITH_TIMEOUT(QDir(destination).entryList(QDir::Files).size(), 2, 10000);
        QVERIFY(waitIdle(window));

        // Cortar y pegar mueve, y lo cortado se pega una sola vez.
        QTRY_VERIFY_WITH_TIMEOUT(pane->model()->rowOfName(QStringLiteral("nota.txt")) >= 0, 10000);
        QVERIFY(waitReady(pane));
        selectNames(pane, {QStringLiteral("nota.txt")});
        window.cutSelection();
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("otra")));
        pane->navigateTo(directory.filePath(QStringLiteral("otra")));
        QVERIFY(waitReady(pane));
        window.paste();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath(QStringLiteral("otra/nota.txt"))), 10000);
        QVERIFY(!QFileInfo::exists(destination + QStringLiteral("/nota.txt")));
        QVERIFY(waitIdle(window));
        QVERIFY(fileSelectionFrom(QGuiApplication::clipboard()->mimeData()).isEmpty());

        // Deshacer devuelve el archivo a donde estaba.
        window.undo();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(destination + QStringLiteral("/nota.txt")), 10000);
        QVERIFY(waitIdle(window));
    }

    void trashAndPermanentDelete()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("a.txt")), 5);
        writeFile(directory.filePath(QStringLiteral("b.txt")), 5);
        MainWindow window(m_scheduler, openJournal(data));
        window.show();
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));

        // Si el volumen temporal no admite papelera, la ventana pregunta por
        // la eliminación permanente; aquí se contesta que no.
        int questions = 0;
        window.setDeleteConfirmation([&questions](const QStringList &) { ++questions; return false; });

        selectNames(pane, {QStringLiteral("a.txt")});
        QSignalSpy reports(&window, &MainWindow::actionReported);
        window.trashSelection();
        QTRY_VERIFY_WITH_TIMEOUT(reports.count() >= 1, 10000);
        QVERIFY(waitIdle(window));
        if (questions == 0) {
            // Fue a la papelera: desaparece de la carpeta y Deshacer lo devuelve.
            QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("a.txt"))));
            QTRY_COMPARE_WITH_TIMEOUT(pane->model()->rowCount(), 1, 10000);
            window.undo();
            QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath(QStringLiteral("a.txt"))), 10000);
            QVERIFY(waitIdle(window));
        } else {
            QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("a.txt"))));
        }

        // Eliminación permanente: sin confirmar no se borra nada.
        QTRY_COMPARE_WITH_TIMEOUT(pane->model()->rowCount(), 2, 10000);
        QVERIFY(waitReady(pane));
        selectNames(pane, {QStringLiteral("b.txt")});
        questions = 0;
        window.deleteSelectionPermanently();
        QCOMPARE(questions, 1);
        QVERIFY(waitIdle(window));
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("b.txt"))));

        // Confirmando, se borra.
        window.setDeleteConfirmation([](const QStringList &paths) { return paths.size() == 1; });
        window.deleteSelectionPermanently();
        QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(directory.filePath(QStringLiteral("b.txt"))), 10000);
        QVERIFY(waitIdle(window));
        QTRY_COMPARE_WITH_TIMEOUT(pane->model()->rowCount(), 1, 10000);
    }

    void dragAndDrop()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("carpeta")));
        writeFile(directory.filePath(QStringLiteral("a.txt")), 5);
        writeFile(directory.filePath(QStringLiteral("b.txt")), 5);
        const QString folder = directory.filePath(QStringLiteral("carpeta"));

        MainWindow window(m_scheduler, openJournal(data));
        window.resize(900, 600);
        window.show();
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        QTreeView *view = pane->view();

        // Lo que la lista entrega al arrastrar: las rutas de lo seleccionado.
        selectNames(pane, {QStringLiteral("a.txt"), QStringLiteral("b.txt")});
        std::unique_ptr<QMimeData> dragged(pane->model()->mimeData(view->selectionModel()->selectedIndexes()));
        QVERIFY(dragged != nullptr);
        QCOMPARE(fileSelectionFrom(dragged.get()).paths.size(), 2);

        const auto dropAt = [&](const QMimeData *mime, const QPoint &position, Qt::KeyboardModifiers modifiers) {
            QDragEnterEvent enter(position, Qt::CopyAction | Qt::MoveAction, mime, Qt::LeftButton, modifiers);
            QApplication::sendEvent(view->viewport(), &enter);
            QDragMoveEvent move(position, Qt::CopyAction | Qt::MoveAction, mime, Qt::LeftButton, modifiers);
            QApplication::sendEvent(view->viewport(), &move);
            if (!move.isAccepted()) {
                QDragLeaveEvent leave;
                QApplication::sendEvent(view->viewport(), &leave);
                return false;
            }
            QDropEvent drop(position, Qt::CopyAction | Qt::MoveAction, mime, Qt::LeftButton, modifiers);
            QApplication::sendEvent(view->viewport(), &drop);
            // Al origen nunca se le dice "movido": el original lo quita el motor.
            return drop.isAccepted() && drop.dropAction() == Qt::CopyAction;
        };
        const auto centerOf = [&](const QString &name) {
            return view->visualRect(pane->model()->index(pane->model()->rowOfName(name), 0)).center();
        };
        const QPoint emptyArea(view->viewport()->width() / 2, view->viewport()->height() - 5);

        // Mayús + soltar sobre el espacio vacío de la misma carpeta: no hay nada que mover.
        QVERIFY(!dropAt(dragged.get(), emptyArea, Qt::ShiftModifier));

        // Ctrl + soltar sobre la carpeta: copia dentro de ella.
        QVERIFY(dropAt(dragged.get(), centerOf(QStringLiteral("carpeta")), Qt::ControlModifier));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(folder + QStringLiteral("/a.txt")), 10000);
        QVERIFY(waitIdle(window));
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("a.txt"))));
        QVERIFY(QFileInfo::exists(folder + QStringLiteral("/b.txt")));

        // Mayús + soltar sobre la carpeta: mueve. Ya hay "a.txt" dentro: se
        // pregunta; con «Omitir», no se sobrescribe y el original sigue en su sitio.
        window.setCollisionAnswerForTesting([](const ops::Conflict &, const tasks::CancelToken &) {
            return ops::CollisionDecision{ops::CollisionPolicy::Skip, false, false};
        });
        QVERIFY(waitReady(pane));
        std::unique_ptr<QMimeData> onlyA(makeFileMimeData({directory.filePath(QStringLiteral("a.txt"))}, false));
        QSignalSpy reports(&window, &MainWindow::actionReported);
        QVERIFY(dropAt(onlyA.get(), centerOf(QStringLiteral("carpeta")), Qt::ShiftModifier));
        QTRY_VERIFY_WITH_TIMEOUT(reports.count() >= 1, 10000);
        QVERIFY(waitIdle(window));
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("a.txt"))));

        // Archivos que llegan de fuera (otra carpeta) y se sueltan en el espacio vacío.
        QTemporaryDir outside;
        writeFile(outside.filePath(QStringLiteral("externo.txt")), 7);
        std::unique_ptr<QMimeData> external(makeFileMimeData({outside.filePath(QStringLiteral("externo.txt"))}, false));
        QVERIFY(waitReady(pane));
        QVERIFY(dropAt(external.get(), emptyArea, Qt::ShiftModifier));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath(QStringLiteral("externo.txt"))), 10000);
        QVERIFY(waitIdle(window));
        QVERIFY(!QFileInfo::exists(outside.filePath(QStringLiteral("externo.txt"))));

        // Mientras la carpeta se vuelve a leer, soltar en ella también vale
        // (en macOS el aviso del disco llega tarde y la relectura coincide).
        writeFile(outside.filePath(QStringLiteral("durante.txt")), 7);
        std::unique_ptr<QMimeData> during(makeFileMimeData({outside.filePath(QStringLiteral("durante.txt"))}, false));
        pane->refresh();
        QCOMPARE(pane->model()->state(), DirectoryModel::State::Loading);
        QVERIFY(dropAt(during.get(), emptyArea, Qt::ShiftModifier));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath(QStringLiteral("durante.txt"))), 10000);
        QVERIFY(waitIdle(window));

        // Una carpeta no se puede soltar dentro de sí misma, ni texto que no son archivos.
        // Dos relecturas seguidas conservan las filas.
        pane->refresh();
        pane->refresh();
        QVERIFY(pane->model()->rowOfName(QStringLiteral("carpeta")) >= 0);
        QVERIFY(waitReady(pane));
        std::unique_ptr<QMimeData> itself(makeFileMimeData({folder}, false));
        QVERIFY(!dropAt(itself.get(), centerOf(QStringLiteral("carpeta")), Qt::ControlModifier));
        QMimeData text;
        text.setText(QStringLiteral("hola"));
        QVERIFY(!dropAt(&text, emptyArea, Qt::ControlModifier));
    }

    void bulkRenameFromF2WithPreviewAndUndo()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("IMG_001.JPG")), 10);
        writeFile(directory.filePath(QStringLiteral("IMG_002.JPG")), 10);
        writeFile(directory.filePath(QStringLiteral("nota.txt")), 10);
        MainWindow window(m_scheduler, openJournal(data));
        window.show();
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        selectNames(pane, {QStringLiteral("IMG_001.JPG"), QStringLiteral("IMG_002.JPG")});

        QStringList shown;
        window.setBulkRenameDriverForTesting([&](BulkRenameDialog &dialog) {
            rename::Rule replace;
            replace.kind = rename::Rule::Kind::Replace;
            replace.find = QStringLiteral("IMG_");
            replace.with = QStringLiteral("Viaje ");
            dialog.setRules({replace});
            dialog.setExtensionMode(rename::ExtensionMode::Lower);
            if (!QTest::qWaitFor([&] { return dialog.isPreviewCurrent(); }, 10000)) {
                return false;
            }
            for (int row = 0; row < dialog.preview()->topLevelItemCount(); ++row) {
                shown.append(dialog.preview()->topLevelItem(row)->text(0) + QStringLiteral(" -> ")
                             + dialog.preview()->topLevelItem(row)->text(1));
            }
            return dialog.renameButton()->isEnabled();
        });
        // F2 de verdad, sobre la lista, con dos seleccionados: renombrado masivo.
        pane->view()->setFocus();
        QTest::keyClick(pane->view(), Qt::Key_F2);
        QCOMPARE(shown, (QStringList{QStringLiteral("IMG_001.JPG -> Viaje 001.jpg"),
                                     QStringLiteral("IMG_002.JPG -> Viaje 002.jpg")}));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath(QStringLiteral("Viaje 002.jpg"))), 10000);
        QVERIFY(waitIdle(window));
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("Viaje 001.jpg"))));
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("nota.txt"))));

        // Deshacer devuelve los dos de una vez.
        window.undo();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath(QStringLiteral("IMG_001.JPG"))), 10000);
        QVERIFY(waitIdle(window));
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("IMG_002.JPG"))));

        // Toda la carpeta, con contador: incluye nota.txt.
        window.setBulkRenameDriverForTesting([&](BulkRenameDialog &dialog) {
            rename::Rule counter;
            counter.kind = rename::Rule::Kind::Counter;
            counter.count = 2;
            counter.separator = QStringLiteral(" - ");
            dialog.setRules({counter});
            dialog.setScope(BulkRenameDialog::Scope::Folder, false);
            return QTest::qWaitFor([&] { return dialog.isPreviewCurrent(); }, 10000);
        });
        window.showBulkRename();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath(QStringLiteral("03 - nota.txt"))), 10000);
        QVERIFY(waitIdle(window));
    }

    void organizeShowsTheSimulationAndUndoesTogether()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("informe.pdf")), 10);
        writeFile(directory.filePath(QStringLiteral("foto.jpg")), 10);
        writeFile(directory.filePath(QStringLiteral("raro.xyz")), 10);
        // Una subcarpeta con solo fotos: al sacarlas queda vacía y se quita.
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("viaje")));
        writeFile(directory.filePath(QStringLiteral("viaje/playa.jpg")), 10);
        MainWindow window(m_scheduler, openJournal(data));
        window.show();
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        QStringList topLevel;
        window.setOrganizeDriverForTesting([&](OrganizeDialog &dialog) {
            // Con las reglas de fábrica. Organizar solo se puede con la simulación a la vista.
            if (!QTest::qWaitFor([&] { return dialog.isSimulationCurrent(); }, 10000)) {
                return false;
            }
            for (int row = 0; row < dialog.simulation()->topLevelItemCount(); ++row) {
                topLevel.append(dialog.simulation()->topLevelItem(row)->text(0));
            }
            // Cambiar algo invalida la simulación hasta que se rehace.
            dialog.setRecursive(true);
            if (dialog.applyButton()->isEnabled()) {
                return false;
            }
            return QTest::qWaitFor([&] { return dialog.isSimulationCurrent() && dialog.applyButton()->isEnabled(); },
                                   10000);
        });
        window.showOrganize();
        topLevel.sort();
        QCOMPARE(topLevel, (QStringList{QStringLiteral("Documentos"), QStringLiteral("Imágenes")}));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath(QStringLiteral("Documentos/PDF/informe.pdf"))),
                                 10000);
        QVERIFY(waitIdle(window));
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("Imágenes/JPG/foto.jpg"))));
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("raro.xyz"))));
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("Imágenes/JPG/playa.jpg"))));
        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("viaje"))));
        window.undo();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath(QStringLiteral("informe.pdf"))), 10000);
        QVERIFY(waitIdle(window));
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("foto.jpg"))));
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("viaje/playa.jpg"))));
        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("Documentos"))));
    }

    void contextMenuChangesTheView()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("a.txt")), 10);
        MainWindow window(m_scheduler, openJournal(data));
        window.show();
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        QCOMPARE(pane->viewMode(), BrowserPane::ViewMode::Details);
        // Con y sin algo seleccionado, el menú trae «Vista» con las tres vistas.
        for (const bool withSelection : {true, false}) {
            if (withSelection) {
                selectNames(pane, {QStringLiteral("a.txt")});
            } else {
                pane->view()->clearSelection();
            }
            QStringList views;
            QTimer::singleShot(100, &window, [&views] {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (menu == nullptr) {
                    return;
                }
                for (QAction *action : menu->actions()) {
                    if (action->menu() != nullptr && action->text() == QStringLiteral("Vista")) {
                        for (QAction *inner : action->menu()->actions()) {
                            views.append(inner->text());
                        }
                        // Elegir «Columnas» desde el menú.
                        for (QAction *inner : action->menu()->actions()) {
                            if (inner->text() == QStringLiteral("Columnas")) {
                                inner->trigger();
                            }
                        }
                    }
                }
                menu->close();
            });
            emit pane->contextMenuRequested(pane->view()->viewport()->mapToGlobal(QPoint(10, 10)));
            QVERIFY2(views.contains(QStringLiteral("Detalles")) && views.contains(QStringLiteral("Miniaturas"))
                         && views.contains(QStringLiteral("Columnas")),
                     qPrintable(views.join(QStringLiteral(", "))));
            QCOMPARE(pane->viewMode(), BrowserPane::ViewMode::Columns);
            window.setViewMode(BrowserPane::ViewMode::Details);
        }
    }

    void duplicatesAreMarkedConfirmedAndTrashed()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        QByteArray content(50000, 'd');
        for (const QString &name : {QStringLiteral("a/foto.jpg"), QStringLiteral("b/foto.jpg"), QStringLiteral("c/copia.jpg")}) {
            QDir().mkpath(QFileInfo(directory.filePath(name)).absolutePath());
            QFile file(directory.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(content);
        }
        writeFile(directory.filePath(QStringLiteral("unico.txt")), 10);
        MainWindow window(m_scheduler, openJournal(data));
        window.show();
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        int groups = -1;
        bool keptOne = false;
        window.setDuplicatesDriverForTesting([&](DuplicatesDialog &dialog) {
            dialog.startSearch();
            if (!QTest::qWaitFor([&] { return !dialog.isSearching(); }, 20000)) {
                return false;
            }
            groups = dialog.results()->topLevelItemCount();
            // Marcar a mano las tres: la tercera se desmarca sola.
            QTreeWidgetItem *group = dialog.results()->topLevelItem(0);
            for (int i = 0; i < group->childCount(); ++i) {
                group->child(i)->setCheckState(0, Qt::Checked);
            }
            keptOne = dialog.marked().size() == 2;
            // Con la regla: todas menos la de ruta más corta (a/foto.jpg o b/foto.jpg).
            dialog.markWithRule(dupes::Keep::ShortestPath);
            if (dialog.marked().size() != 2) {
                return false;
            }
            dialog.setConfirmForTesting([](int files, qint64 bytes) { return files == 2 && bytes == 100000; });
            for (QPushButton *button : dialog.findChildren<QPushButton *>()) {
                if (button->text().startsWith(QStringLiteral("Enviar"))) {
                    button->click();
                }
            }
            return dialog.result() == QDialog::Accepted;
        });
        // Donde la carpeta temporal no tiene papelera (macOS), se ofrecería
        // borrar del todo: se contesta que no y la prueba se salta ahí.
        bool trashUnsupported = false;
        window.setDeleteConfirmation([&trashUnsupported](const QStringList &) {
            trashUnsupported = true;
            return false;
        });
        window.showDuplicates();
        QCOMPARE(groups, 1);
        QVERIFY(keptOne);
        const auto remaining = [&] {
            return int(QFileInfo::exists(directory.filePath(QStringLiteral("a/foto.jpg"))))
                + int(QFileInfo::exists(directory.filePath(QStringLiteral("b/foto.jpg"))))
                + int(QFileInfo::exists(directory.filePath(QStringLiteral("c/copia.jpg"))));
        };
        QTRY_VERIFY_WITH_TIMEOUT(trashUnsupported || remaining() == 1, 10000);
        QVERIFY(waitIdle(window));
        if (trashUnsupported) {
            QSKIP("El volumen temporal de este equipo no admite papelera.");
        }
        // Deshacer las devuelve.
        window.undo();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath(QStringLiteral("c/copia.jpg"))), 10000);
        QVERIFY(waitIdle(window));
    }

    void similarImagesAreComparedAndOnlyMarkedOnesTrashed()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        QImage photo(640, 480, QImage::Format_RGB32);
        {
            QPainter painter(&photo);
            QLinearGradient gradient(0, 0, 640, 480);
            gradient.setColorAt(0, Qt::yellow);
            gradient.setColorAt(1, Qt::darkBlue);
            painter.fillRect(photo.rect(), gradient);
            painter.setBrush(Qt::red);
            painter.drawEllipse(QPoint(200, 150), 80, 60);
        }
        QVERIFY(photo.save(directory.filePath(QStringLiteral("original.png"))));
        QVERIFY(photo.save(directory.filePath(QStringLiteral("comprimida.jpg")), "JPG", 40));
        QVERIFY(photo.scaled(200, 150).save(directory.filePath(QStringLiteral("pequena.jpg")), "JPG", 80));
        MainWindow window(m_scheduler, openJournal(data));
        window.show();
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        int groups = -1;
        bool lastRefused = false;
        window.setSimilarDriverForTesting([&](SimilarImagesDialog &dialog) {
            dialog.startSearch();
            if (!QTest::qWaitFor([&] { return !dialog.isSearching(); }, 30000)) {
                return false;
            }
            groups = dialog.groupCount();
            if (groups != 1) {
                return false;
            }
            const QList<similar::Image> images = dialog.groups().first().images;
            // La primera es la de más bytes; se marcan las otras dos y la última no se deja.
            dialog.setMarked(images[1].path, true);
            dialog.setMarked(images[2].path, true);
            lastRefused = !dialog.setMarked(images[0].path, true);
            dialog.setConfirmForTesting([](int files, qint64) { return files == 2; });
            dialog.sendToTrash();
            return dialog.result() == QDialog::Accepted;
        });
        bool trashUnsupported = false;
        window.setDeleteConfirmation([&trashUnsupported](const QStringList &) {
            trashUnsupported = true;
            return false;
        });
        window.showSimilarImages();
        QCOMPARE(groups, 1);
        QVERIFY(lastRefused);
        QTRY_VERIFY_WITH_TIMEOUT(trashUnsupported || QDir(directory.path()).entryList(QDir::Files).size() == 1, 10000);
        QVERIFY(waitIdle(window));
        if (trashUnsupported) {
            QSKIP("El volumen temporal de este equipo no admite papelera.");
        }
    }

    void tagsShowFollowRenamesAndAreSearchable()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("factura.pdf")), 10);
        writeFile(directory.filePath(QStringLiteral("otra.pdf")), 10);
        MainWindow window(m_scheduler, openJournal(data));
        window.show();
        QVERIFY(window.tagStore() != nullptr);
        const auto created = window.tagStore()->createTag(QStringLiteral("Prueba %1").arg(QCoreApplication::applicationPid()),
                                                          QStringLiteral("#e74c3c"));
        QVERIFY(created.ok());
        const tags::Tag tag = created.value();
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        window.setTag({directory.filePath(QStringLiteral("factura.pdf"))}, tag.id, true);
        const auto tagCell = [&](const QString &name) {
            const int row = pane->model()->rowOfName(name);
            return row < 0 ? QString()
                           : pane->model()->data(pane->model()->index(row, int(Column::Tags))).toString();
        };
        QTRY_COMPARE_WITH_TIMEOUT(tagCell(QStringLiteral("factura.pdf")), tag.name, 10000);
        QCOMPARE(tagCell(QStringLiteral("otra.pdf")), QString());

        // Renombrar desde la aplicación: la etiqueta va con el archivo; deshacer, también.
        selectNames(pane, {QStringLiteral("factura.pdf")});
        emit pane->renameRequested(directory.filePath(QStringLiteral("factura.pdf")), QStringLiteral("factura 2026.pdf"));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath(QStringLiteral("factura 2026.pdf"))), 10000);
        QVERIFY(waitIdle(window));
        QTRY_COMPARE_WITH_TIMEOUT(window.tagStore()->pathsWithTag(tag.name),
                                  QStringList{QDir::cleanPath(directory.filePath(QStringLiteral("factura 2026.pdf")))}, 10000);
        window.undo();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath(QStringLiteral("factura.pdf"))), 10000);
        QVERIFY(waitIdle(window));
        QTRY_COMPARE_WITH_TIMEOUT(window.tagStore()->pathsWithTag(tag.name),
                                  QStringList{QDir::cleanPath(directory.filePath(QStringLiteral("factura.pdf")))}, 10000);

        // Buscar por etiqueta.
        window.searchEdit()->setText(QStringLiteral("tag:\"%1\"").arg(tag.name));
        QTest::keyClick(window.searchEdit(), Qt::Key_Return);
        QVERIFY(pane->showsSearch());
        QVERIFY(QTest::qWaitFor([pane] { return pane->searchState() == BrowserPane::SearchState::Finished; }, 20000));
        QCOMPARE(pane->resultsModel()->count(), 1);
        QCOMPARE(pane->resultsModel()->hitAt(0).entry.name, QStringLiteral("factura.pdf"));
        window.tagStore()->deleteTag(tag.id);
    }

    void interruptedCopyIsResumedAndClosingMidCopyCanBeResumed()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("fotos")));
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("destino")));
        for (int i = 0; i < 6; ++i) {
            writeFile(directory.filePath(QStringLiteral("fotos/foto%1.raw").arg(i)), 1'000'000);
        }
        const QString source = directory.filePath(QStringLiteral("fotos"));
        const QString destination = directory.filePath(QStringLiteral("destino"));
        QProcess process;
        process.start(QCoreApplication::applicationDirPath() + QStringLiteral("/eu-crash-helper"),
                      {data.filePath(QStringLiteral("historial.db")), source, destination,
                       QStringLiteral("2500000"), QStringLiteral("copiar")});
        QVERIFY(process.waitForFinished(60000));
        QCOMPARE(process.exitCode(), 42);

        // Al abrir se elige reanudar: termina lo que faltaba, sin duplicar.
        {
            MainWindow window(m_scheduler, openJournal(data));
            window.setRecoveryChoice([](const QList<journal::Operation> &) {
                return MainWindow::RecoveryChoice::Resume;
            });
            window.checkInterruptedOperations();
            QTRY_COMPARE_WITH_TIMEOUT(QDir(destination + QStringLiteral("/fotos")).entryList(QDir::Files).size(), 6,
                                      20000);
            QVERIFY(waitIdle(window));
            QCOMPARE(QDir(destination).entryList(QDir::Dirs | QDir::NoDotAndDotDot), QStringList{QStringLiteral("fotos")});
            QCOMPARE(window.transfersPanel()->list()->topLevelItemCount(), 1);
        }
        auto journal = openJournal(data);
        QVERIFY(journal->incompleteOperations().value().isEmpty());
    }

    void interruptedOperationIsOfferedAndReverted()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("fotos")));
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("destino")));
        for (int i = 0; i < 6; ++i) {
            writeFile(directory.filePath(QStringLiteral("fotos/foto%1.raw").arg(i)), 1'000'000);
        }
        const QString destination = directory.filePath(QStringLiteral("destino"));
        const QString journalPath = data.filePath(QStringLiteral("historial.db"));

        // Un proceso aparte empieza a copiar y muere a mitad, como en un corte de luz.
        QProcess process;
        process.start(QCoreApplication::applicationDirPath() + QStringLiteral("/eu-crash-helper"),
                      {journalPath, directory.filePath(QStringLiteral("fotos")), destination,
                       QStringLiteral("2500000"), QStringLiteral("copiar")});
        QVERIFY(process.waitForFinished(60000));
        QCOMPARE(process.exitCode(), 42);
        QVERIFY(!QDir(destination).isEmpty());

        // Primera apertura: el usuario prefiere decidir más tarde. No se toca nada.
        {
            MainWindow window(m_scheduler, openJournal(data));
            int asked = 0;
            window.setRecoveryChoice([&asked](const QList<journal::Operation> &operations) {
                asked = int(operations.size());
                return MainWindow::RecoveryChoice::Later;
            });
            window.checkInterruptedOperations();
            QTRY_COMPARE_WITH_TIMEOUT(asked, 1, 10000);
            QVERIFY(!QDir(destination).isEmpty());
        }

        // Segunda apertura: se vuelve a preguntar y se elige revertir.
        MainWindow window(m_scheduler, openJournal(data));
        QSignalSpy reports(&window, &MainWindow::actionReported);
        window.setRecoveryChoice([](const QList<journal::Operation> &) { return MainWindow::RecoveryChoice::Revert; });
        window.checkInterruptedOperations();
        QTRY_VERIFY_WITH_TIMEOUT(reports.count() >= 1, 20000);
        QVERIFY(waitIdle(window));
        QCOMPARE(reports.last().at(1).toBool(), false);
        // El destino queda limpio y el origen, intacto.
        QVERIFY2(QDir(destination).isEmpty(), qPrintable(QDir(destination).entryList().join(QLatin1Char(','))));
        QCOMPARE(QDir(directory.filePath(QStringLiteral("fotos"))).entryList(QDir::Files).size(), 6);
        QVERIFY(openJournal(data)->incompleteOperations().value().isEmpty());
    }

    void trashWindowRestoresAndPurges()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        const QString keep = directory.filePath(QStringLiteral("recuperar.txt"));
        const QString drop = directory.filePath(QStringLiteral("borrar.txt"));
        writeFile(keep, 11);
        writeFile(drop, 22);
        MainWindow window(m_scheduler, openJournal(data));
        window.show();
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));

        bool trashUnsupported = false;
        window.setDeleteConfirmation([&trashUnsupported](const QStringList &) {
            trashUnsupported = true;
            return false;
        });
        selectNames(pane, {QStringLiteral("recuperar.txt"), QStringLiteral("borrar.txt")});
        window.trashSelection();
        QTRY_VERIFY_WITH_TIMEOUT(trashUnsupported || !QFileInfo::exists(keep), 10000);
        QVERIFY(waitIdle(window));
        if (trashUnsupported) {
            QSKIP("El volumen temporal de este equipo no admite papelera.");
        }

        // La ventana de la papelera muestra los dos elementos (y quizá otros
        // que ya hubiera: la prueba solo toca los suyos).
        TrashDialog *trashWindow = window.showTrash();
        const auto listed = [&](const QString &originalPath) {
            const QList<trash::TrashItem> items = trashWindow->items();
            return std::any_of(items.begin(), items.end(), [&](const trash::TrashItem &item) {
                return item.originalPath == originalPath;
            });
        };
        QTRY_VERIFY_WITH_TIMEOUT(listed(keep) && listed(drop), 10000);

        // Restaurar uno: vuelve a su carpeta con su contenido.
        trashWindow->selectOriginalPaths({keep});
        trashWindow->restoreSelected();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(keep), 10000);
        QCOMPARE(QFileInfo(keep).size(), qint64(11));
        QVERIFY(waitIdle(window));
        QTRY_VERIFY_WITH_TIMEOUT(!trashWindow->isLoading() && !listed(keep) && listed(drop), 10000);

        // Eliminar el otro para siempre: pide confirmación; sin ella no pasa nada.
        int questions = 0;
        bool answer = false;
        window.setDeleteConfirmation([&](const QStringList &paths) {
            ++questions;
            return answer && paths == QStringList{drop};
        });
        trashWindow->selectOriginalPaths({drop});
        trashWindow->purgeSelected();
        QCOMPARE(questions, 1);
        QVERIFY(waitIdle(window));
        trashWindow->reload();
        QTRY_VERIFY_WITH_TIMEOUT(!trashWindow->isLoading(), 10000);
        QVERIFY(listed(drop));

        answer = true;
        trashWindow->selectOriginalPaths({drop});
        trashWindow->purgeSelected();
        QCOMPARE(questions, 2);
        QTRY_VERIFY_WITH_TIMEOUT(!trashWindow->isLoading() && !listed(drop), 10000);
        QVERIFY(waitIdle(window));
        QVERIFY(!QFileInfo::exists(drop));
    }

    void dropPlanRules()
    {
        std::unique_ptr<QMimeData> files(makeFileMimeData({QStringLiteral("/a/uno.txt"), QStringLiteral("/a/dos.txt")}, false));
        const MoveByDefaultRule sameDisk = [](const QString &, const QString &) { return true; };

        // Sin teclas decide la regla; sin regla se copia.
        QVERIFY(planDrop(files.get(), QStringLiteral("/b"), Qt::NoModifier, sameDisk).move);
        QVERIFY(!planDrop(files.get(), QStringLiteral("/b"), Qt::NoModifier, {}).move);
        QVERIFY(!planDrop(files.get(), QStringLiteral("/b"), Qt::ControlModifier, sameDisk).move);
        QVERIFY(planDrop(files.get(), QStringLiteral("/b"), Qt::ShiftModifier, {}).move);
        QCOMPARE(planDrop(files.get(), QStringLiteral("/b"), Qt::NoModifier, sameDisk).paths.size(), 2);

        // Mover a la carpeta donde ya están no es nada; copiar ahí sí (duplica).
        QVERIFY(!planDrop(files.get(), QStringLiteral("/a"), Qt::ShiftModifier, {}).valid);
        QVERIFY(planDrop(files.get(), QStringLiteral("/a"), Qt::ControlModifier, {}).valid);
        // Una carpeta dentro de sí misma, nunca.
        std::unique_ptr<QMimeData> folder(makeFileMimeData({QStringLiteral("/a")}, false));
        QVERIFY(!planDrop(folder.get(), QStringLiteral("/a/sub"), Qt::ControlModifier, {}).valid);
        QVERIFY(!planDrop(folder.get(), QStringLiteral("/a"), Qt::ControlModifier, {}).valid);
        QVERIFY(planDrop(folder.get(), QStringLiteral("/ab"), Qt::ControlModifier, {}).valid);
    }

    void clickingThePathCopiesIt()
    {
        QTemporaryDir directory;
        MainWindow window(m_scheduler);
        window.show();
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        auto *pathBar = window.findChild<PathBar *>();
        QVERIFY(pathBar != nullptr);

        QGuiApplication::clipboard()->clear();
        QTest::mouseClick(pathBar, Qt::LeftButton, Qt::NoModifier, QPoint(pathBar->width() - 4, pathBar->height() / 2));
        QCOMPARE(QGuiApplication::clipboard()->text(), QDir::toNativeSeparators(QDir::cleanPath(directory.path())));
        auto *edit = pathBar->findChild<QLineEdit *>();
        QVERIFY(edit != nullptr);
        QCOMPARE(edit->selectedText(), edit->text());
        QTest::keyClick(edit, Qt::Key_Escape);

        // Si hay archivos copiados esperando a pegarse, no se pisan.
        QGuiApplication::clipboard()->setMimeData(makeFileMimeData({directory.filePath(QStringLiteral("x"))}, true));
        QTest::mouseClick(pathBar, Qt::LeftButton, Qt::NoModifier, QPoint(pathBar->width() - 4, pathBar->height() / 2));
        QVERIFY(fileSelectionFrom(QGuiApplication::clipboard()->mimeData()).cut);
        QGuiApplication::clipboard()->clear();
    }

    void everyRowHasAnIcon()
    {
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("carpeta")));
        writeFile(directory.filePath(QStringLiteral("a.txt")));
        writeFile(directory.filePath(QStringLiteral("sin-extension")));
        writeFile(directory.filePath(QStringLiteral("raro.zzzxq")));
        MainWindow window(m_scheduler);
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        QCOMPARE(pane->model()->rowCount(), 4);
        for (int row = 0; row < pane->model()->rowCount(); ++row) {
            const QIcon icon = pane->model()->index(row, 0).data(Qt::DecorationRole).value<QIcon>();
            QVERIFY2(!icon.isNull() && !icon.pixmap(16, 16).isNull(),
                     qPrintable(pane->model()->entryAt(row).name));
        }
    }

    void filterBarFiltersWhileTyping()
    {
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("Fotos")));
        writeFile(directory.filePath(QStringLiteral("informe marzo.pdf")));
        writeFile(directory.filePath(QStringLiteral("informe abril.pdf")));
        writeFile(directory.filePath(QStringLiteral("notas.txt")));
        MainWindow window(m_scheduler);
        window.show();
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        QVERIFY(!pane->isFilterVisible());

        pane->showFilter();
        QVERIFY(pane->isFilterVisible());
        QTest::keyClicks(pane->filterEdit(), QStringLiteral("informe"));
        QCOMPARE(pane->model()->rowCount(), 2);
        QCOMPARE(pane->filterMessage(), QStringLiteral("2 de 4"));
        QVERIFY(pane->statusText().contains(QStringLiteral("2")));

        // Una consulta a medio escribir explica el problema y no vacía la lista.
        QTest::keyClicks(pane->filterEdit(), QStringLiteral(" (marzo OR"));
        QCOMPARE(pane->model()->rowCount(), 2);
        QVERIFY(pane->filterMessage().contains(QStringLiteral("OR")));
        QTest::keyClicks(pane->filterEdit(), QStringLiteral(" mayo)"));
        QCOMPARE(pane->model()->rowCount(), 1);

        // Un filtro que aún no existe se dice, no se aparenta.
        pane->filterEdit()->setText(QStringLiteral("tag:rojo"));
        QCOMPARE(pane->model()->rowCount(), 0);
        QVERIFY(pane->filterMessage().contains(QStringLiteral("tag")));

        // Esc quita el filtro y vuelve todo.
        QTest::keyClick(pane->filterEdit(), Qt::Key_Escape);
        QVERIFY(!pane->isFilterVisible());
        QCOMPARE(pane->model()->rowCount(), 4);

        // Entrar en otra carpeta también lo quita.
        pane->showFilter();
        pane->filterEdit()->setText(QStringLiteral("type:carpeta"));
        QCOMPARE(pane->model()->rowCount(), 1);
        pane->navigateTo(directory.filePath(QStringLiteral("Fotos")));
        QVERIFY(waitReady(pane));
        QVERIFY(!pane->isFilterVisible());
        pane->goBack();
        QVERIFY(waitReady(pane));
        QCOMPARE(pane->model()->rowCount(), 4);
    }

    void volumesLocationListsDrivesWithTheirSpace()
    {
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("discoA/dentro")));
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("discoB")));
        const QString rootA = QDir::cleanPath(directory.filePath(QStringLiteral("discoA")));
        const QString rootB = QDir::cleanPath(directory.filePath(QStringLiteral("discoB")));

        fs::Volume a;
        a.rootPath = rootA;
        a.displayName = QStringLiteral("Datos");
        a.fileSystem = QStringLiteral("ext4");
        a.kind = fs::DeviceKind::Ssd;
        a.totalBytes = 1000000000;   // 1 GB
        a.freeBytes = 250000000;     // 250 MB libres -> 75 % ocupado
        fs::Volume b;
        b.rootPath = rootB;
        b.displayName = QStringLiteral("USB");
        b.fileSystem = QStringLiteral("exfat");
        b.kind = fs::DeviceKind::Removable;
        b.totalBytes = 16000000000;
        b.freeBytes = 16000000000;

        MainWindow window(m_scheduler, openJournal(directory));
        BrowserPane *pane = window.openTab(rootA + QStringLiteral("/dentro"));
        QVERIFY(waitReady(pane));
        pane->setVolumeSource([a, b] { return QList<fs::Volume>{a, b}; });
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        // Un clic en el rótulo "Unidades" del árbol lleva a la lista.
        Sidebar *sidebar = window.sidebar();
        QModelIndex section;
        for (int row = 0; row < sidebar->model()->rowCount(); ++row) {
            if (sidebar->model()->index(row, 0).data().toString() == QStringLiteral("Unidades")) {
                section = sidebar->model()->index(row, 0);
            }
        }
        QVERIFY(section.isValid());
        QTest::mouseClick(sidebar->viewport(), Qt::LeftButton, Qt::NoModifier, sidebar->visualRect(section).center());
        QVERIFY(pane->showsVolumes());
        QCOMPARE(pane->currentPath(), volumesLocation());
        VolumesView *volumes = pane->volumesView();
        QTRY_COMPARE(volumes->topLevelItemCount(), 2);
        QVERIFY(volumes->isVisible());
        QVERIFY(!pane->view()->isVisible());

        // Tamaño total, ocupado, libre y porcentaje de cada unidad.
        QTreeWidgetItem *first = volumes->topLevelItem(0);
        QCOMPARE(first->text(VolumesView::NameColumn), QStringLiteral("Datos"));
        QCOMPARE(first->text(VolumesView::LocationColumn), QDir::toNativeSeparators(rootA));
        QVERIFY(first->text(VolumesView::TypeColumn).contains(QStringLiteral("SSD")));
        QVERIFY(first->text(VolumesView::TypeColumn).contains(QStringLiteral("ext4")));
        QCOMPARE(first->text(VolumesView::TotalColumn), core::formatSize(1000000000));
        QCOMPARE(first->text(VolumesView::UsedColumn), core::formatSize(750000000));
        QCOMPARE(first->text(VolumesView::FreeColumn), core::formatSize(250000000));
        QVERIFY(first->text(VolumesView::UsageColumn).startsWith(QStringLiteral("75")));
        QVERIFY(!first->icon(VolumesView::NameColumn).isNull());
        QVERIFY(volumes->topLevelItem(1)->text(VolumesView::UsageColumn).startsWith(QStringLiteral("0")));

        // Ordenar por espacio libre usa el número, no el texto.
        volumes->sortByColumn(VolumesView::FreeColumn, Qt::DescendingOrder);
        QCOMPARE(volumes->topLevelItem(0)->text(VolumesView::NameColumn), QStringLiteral("USB"));
        volumes->sortByColumn(VolumesView::NameColumn, Qt::AscendingOrder);

        // Aquí no hay carpeta: nada que copiar, crear ni pegar.
        QVERIFY(pane->selectedPaths().isEmpty());
        QVERIFY(!pane->canGoUp());
        QVERIFY(pane->statusText().contains(QStringLiteral("2 unidad")));
        window.newFolder();
        QVERIFY(!window.fileActions()->isBusy());
        pane->showFilter();
        QVERIFY(!pane->isFilterVisible());
        QCOMPARE(PathBar::crumbsFor(volumesLocation()).size(), 1);

        // Cambiar las unidades de tamaño cambia el texto de la lista.
        pane->setSizeUnits(core::SizeUnits::Binary);
        QCOMPARE(volumes->topLevelItem(0)->text(VolumesView::TotalColumn),
                 core::formatSize(1000000000, core::SizeUnits::Binary));
        pane->setSizeUnits(core::SizeUnits::Decimal);

        // Abrir una unidad entra en su raíz; subir desde la raíz vuelve a la
        // lista con esa unidad seleccionada.
        volumes->setCurrentItem(volumes->topLevelItem(0));
        QVERIFY(pane->statusText().contains(QStringLiteral("Datos")));
        emit volumes->itemActivated(volumes->topLevelItem(0), 0);
        QCOMPARE(pane->currentPath(), rootA);
        QVERIFY(!pane->showsVolumes());
        QVERIFY(waitReady(pane));
        QVERIFY(pane->view()->isVisible());
        QVERIFY(pane->model()->rowOfName(QStringLiteral("dentro")) >= 0);

        pane->goBack();
        QVERIFY(pane->showsVolumes());
        QTRY_COMPARE(volumes->selectedRoot(), rootA);
        pane->goForward();
        QCOMPARE(pane->currentPath(), rootA);
        QVERIFY(waitReady(pane));

        // La pestaña en "Unidades" se recuerda entre sesiones.
        pane->navigateTo(volumesLocation());
        QVERIFY(pane->showsVolumes());
        window.close();
        MainWindow restored(m_scheduler);
        restored.restoreSession();
        QCOMPARE(restored.tabCount(), 1);
        QVERIFY(restored.currentPane()->showsVolumes());
        QSettings().clear();
    }

    void goingUpFromARootShowsTheVolumes()
    {
        MainWindow window(m_scheduler);
        BrowserPane *pane = window.openTab(QDir::rootPath());
        QVERIFY(waitReady(pane));
        QVERIFY(pane->canGoUp());
        pane->goUp();
        QVERIFY(pane->showsVolumes());
        QVERIFY(!pane->canGoUp());
        QVERIFY(pane->canGoBack());
        // Las unidades reales del equipo: al menos la del sistema, con tamaño.
        QTRY_VERIFY(pane->volumesView()->topLevelItemCount() >= 1);
        pane->goBack();
        QCOMPARE(pane->currentPath(), QDir::cleanPath(QDir::rootPath()));
    }

    void backspaceAndLeftArrowGoUp()
    {
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        QVERIFY(QDir(root).mkpath(QStringLiteral("dentro/más")));
        writeFile(root + QStringLiteral("/dentro/más/a.txt"));

        MainWindow window(m_scheduler, openJournal(directory));
        BrowserPane *pane = window.openTab(root + QStringLiteral("/dentro/más"));
        QVERIFY(waitReady(pane));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        pane->view()->setFocus();

        QTest::keyClick(pane->view(), Qt::Key_Backspace);
        QTRY_COMPARE(pane->currentPath(), root + QStringLiteral("/dentro"));
        QVERIFY(waitReady(pane));
        QTest::keyClick(pane->view(), Qt::Key_Left);
        QTRY_COMPARE(pane->currentPath(), root);
        QVERIFY(waitReady(pane));
        // Al subir queda marcada la carpeta de la que se venía.
        QTRY_COMPARE(pane->model()->entryAt(pane->view()->currentIndex().row()).name, QStringLiteral("dentro"));
    }

    void millerColumnsNavigateWithKeysAndClicks()
    {
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        QVERIFY(QDir(root).mkpath(QStringLiteral("a/b/c")));
        writeFile(root + QStringLiteral("/a/b/c/primero.txt"));
        writeFile(root + QStringLiteral("/a/b/c/segundo.txt"));
        writeFile(root + QStringLiteral("/a/b/foto.png"));
        writeFile(root + QStringLiteral("/a/x.txt"));

        MainWindow window(m_scheduler, openJournal(directory));
        BrowserPane *pane = window.openTab(root + QStringLiteral("/a/b"));
        QVERIFY(waitReady(pane));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.setViewMode(BrowserPane::ViewMode::Columns);
        MillerView *miller = pane->millerView();
        QVERIFY(miller->isVisible());
        QCOMPARE(pane->fileView(), static_cast<QAbstractItemView *>(miller->currentColumn()));

        // Una columna por cada carpeta de la ruta, desde la raíz; en la de
        // arriba queda marcada la carpeta por la que se pasa.
        QStringList columns = miller->columnPaths();
        QCOMPARE(columns.last(), root + QStringLiteral("/a/b"));
        QCOMPARE(columns.at(columns.size() - 2), root + QStringLiteral("/a"));
        QCOMPARE(QDir(columns.first()).isRoot(), true);
        MillerColumn *upper = miller->columnFor(root + QStringLiteral("/a"));
        QVERIFY(upper != nullptr);
        QTRY_COMPARE(upper->currentIndex().data().toString(), QStringLiteral("b"));

        // Con el cursor en una subcarpeta, su contenido se ve a la derecha.
        selectNames(pane, {QStringLiteral("c")});
        QTRY_COMPARE(miller->columnPaths().last(), root + QStringLiteral("/a/b/c"));
        MillerColumn *child = miller->columnFor(root + QStringLiteral("/a/b/c"));
        QVERIFY(child != nullptr);
        QTRY_COMPARE(child->model()->rowCount(), 2);
        // Con un archivo, no.
        selectNames(pane, {QStringLiteral("foto.png")});
        QTRY_COMPARE(miller->columnPaths().last(), root + QStringLiteral("/a/b"));

        // → entra en la carpeta y deja seleccionado lo primero; ← vuelve con
        // la carpeta marcada.
        selectNames(pane, {QStringLiteral("c")});
        miller->currentColumn()->setFocus();
        QTest::keyClick(miller->currentColumn(), Qt::Key_Right);
        QTRY_COMPARE(pane->currentPath(), root + QStringLiteral("/a/b/c"));
        QVERIFY(waitReady(pane));
        QTRY_COMPARE(pane->selectedPaths(), (QStringList{root + QStringLiteral("/a/b/c/primero.txt")}));
        QCOMPARE(miller->columnPaths().last(), root + QStringLiteral("/a/b/c"));
        QTest::keyClick(miller->currentColumn(), Qt::Key_Left);
        QTRY_COMPARE(pane->currentPath(), root + QStringLiteral("/a/b"));
        QVERIFY(waitReady(pane));
        QTRY_COMPARE(pane->selectedPaths(), (QStringList{root + QStringLiteral("/a/b/c")}));

        // Un clic en una columna de la izquierda va a esa carpeta con lo
        // pulsado seleccionado (y Atrás vuelve).
        upper = miller->columnFor(root + QStringLiteral("/a"));
        QTRY_VERIFY(upper->model()->rowCount() == 2);
        const QModelIndex xIndex = upper->model()->index(
            static_cast<DirectoryModel *>(upper->model())->rowOfName(QStringLiteral("x.txt")), 0);
        QTest::mouseClick(upper->viewport(), Qt::LeftButton, {}, upper->visualRect(xIndex).center());
        QTRY_COMPARE(pane->currentPath(), root + QStringLiteral("/a"));
        QVERIFY(waitReady(pane));
        QTRY_COMPARE(pane->selectedPaths(), (QStringList{root + QStringLiteral("/a/x.txt")}));
        QCOMPARE(miller->columnPaths().last(), root + QStringLiteral("/a"));
        pane->goBack();
        QTRY_COMPARE(pane->currentPath(), root + QStringLiteral("/a/b"));

        // La vista se recuerda al cerrar.
        window.close();
        QCOMPARE(QSettings().value(QStringLiteral("view/mode")).toString(), QStringLiteral("columnas"));
        QSettings().setValue(QStringLiteral("view/mode"), QStringLiteral("detalles"));   // para las demás pruebas
    }

    void dualPaneSwitchesCopiesAndIsRestored()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        const QString left = QDir::cleanPath(directory.path()) + QStringLiteral("/izquierda");
        const QString right = QDir::cleanPath(directory.path()) + QStringLiteral("/derecha");
        QVERIFY(QDir().mkpath(left));
        QVERIFY(QDir().mkpath(right));
        writeFile(left + QStringLiteral("/a.txt"), 10);
        {
            MainWindow window(m_scheduler, openJournal(data));
            BrowserPane *pane = window.openTab(left);
            QVERIFY(waitReady(pane));
            window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            QVERIFY(!window.isDualPane());
            QCOMPARE(window.otherPane(), nullptr);
            // Los atajos de ventana necesitan que esté activa (con xcb sin gestor de ventanas, no lo está sola).
            window.activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(&window));

            // F3 divide: el otro panel empieza en la misma carpeta y el teclado sigue aquí.
            pane->fileView()->setFocus();
            QTest::keyClick(pane->fileView(), Qt::Key_F3);
            QVERIFY(window.isDualPane());
            QCOMPARE(window.currentPane(), pane);
            BrowserPane *other = window.otherPane();
            QVERIFY(other != nullptr && other != pane);
            QCOMPARE(other->currentPath(), left);
            other->navigateTo(right);
            QVERIFY(waitReady(other));

            // Mayús+F5 copia lo seleccionado a la carpeta del otro panel.
            selectNames(pane, {QStringLiteral("a.txt")});
            QTest::keyClick(pane->fileView(), Qt::Key_F5, Qt::ShiftModifier);
            QTRY_VERIFY(QFile::exists(right + QStringLiteral("/a.txt")));
            QVERIFY(QFile::exists(left + QStringLiteral("/a.txt")));
            QTRY_VERIFY(!window.fileActions()->isBusy());

            // F6 pasa al otro panel: lo demás de la ventana lo sigue.
            QTest::keyClick(pane->fileView(), Qt::Key_F6);
            QCOMPARE(window.currentPane(), other);
            QCOMPARE(window.otherPane(), pane);
            QTRY_VERIFY(other->fileView()->hasFocus());
            QCOMPARE(window.allPanes().size(), 2);

            // Una pestaña más a la derecha; al cerrar se guarda todo.
            window.openTab(left);
            QCOMPARE(window.tabCount(), 2);   // las del panel activo
            QCOMPARE(window.allPanes().size(), 3);
            QVERIFY(window.close());
        }
        {
            MainWindow restored(m_scheduler, openJournal(data));
            restored.restoreSession();
            QVERIFY(restored.isDualPane());
            QCOMPARE(restored.allPanes().size(), 3);
            // Activo el derecho (como al cerrar), con su pestaña elegida.
            QCOMPARE(restored.currentPane()->currentPath(), left);
            QCOMPARE(restored.tabCount(), 2);
            QCOMPARE(restored.otherPane()->currentPath(), left);

            // Cerrar las del panel derecho deja un solo panel.
            restored.closeTab(1);
            restored.closeTab(0);
            QVERIFY(!restored.isDualPane());
            QCOMPARE(restored.allPanes().size(), 1);
            QCOMPARE(restored.currentPane()->currentPath(), left);
            restored.show();
            QVERIFY(restored.close());   // sesión de un panel, para las demás pruebas
        }
    }

    void syncDialogComparesSimulatesAndSyncs()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        const QString left = QDir::cleanPath(directory.path()) + QStringLiteral("/origen");
        const QString right = QDir::cleanPath(directory.path()) + QStringLiteral("/copia");
        QVERIFY(QDir().mkpath(left + QStringLiteral("/fotos")));
        QVERIFY(QDir().mkpath(right));
        writeFile(left + QStringLiteral("/nuevo.txt"), 10);
        writeFile(left + QStringLiteral("/fotos/a.jpg"), 20);
        writeFile(right + QStringLiteral("/sobra.txt"), 5);

        MainWindow window(m_scheduler, openJournal(data));
        BrowserPane *pane = window.openTab(left);
        QVERIFY(waitReady(pane));
        window.setDualPane(true);
        window.otherPane()->navigateTo(right);
        QVERIFY(waitReady(window.otherPane()));

        bool driven = false;
        window.setSyncDriverForTesting([&](SyncDialog &dialog) {
            driven = true;
            // Con doble panel, compara las carpetas de los dos paneles.
            dialog.startCompare();
            QTRY_VERIFY(!dialog.isBusy());
            QCOMPARE(dialog.comparison().left, left);
            QCOMPARE(dialog.comparison().right, right);
            QCOMPARE(dialog.view()->model()->rowCount(), 3);   // nuevo.txt, fotos y sobra.txt
            QVERIFY(dialog.summary()->text().contains(QStringLiteral("2 solo a la izquierda")));

            // Sin simular no se sincroniza nada.
            dialog.setConfirmForTesting([] { return true; });
            dialog.synchronize();
            QVERIFY(!dialog.isBusy());
            QVERIFY(!QFile::exists(right + QStringLiteral("/nuevo.txt")));

            dialog.simulate();
            QVERIFY(dialog.planIsCurrent());
            QCOMPARE(dialog.plan().count(sync::ActionKind::CopyToRight), 2);
            QCOMPARE(dialog.plan().deletionsNotPropagated, 1);
            // Cambiar una opción obliga a simular otra vez.
            dialog.setPropagateDeletions(true);
            QVERIFY(!dialog.planIsCurrent());
            dialog.simulate();
            QCOMPARE(dialog.plan().count(sync::ActionKind::TrashRight), 1);

            QSignalSpy synced(&dialog, &SyncDialog::syncFinished);
            dialog.synchronize();
            QTRY_COMPARE(synced.count(), 1);
            QTRY_VERIFY(!dialog.isBusy());   // y vuelve a comparar
            QVERIFY(QFile::exists(right + QStringLiteral("/nuevo.txt")));
            QVERIFY(QFile::exists(right + QStringLiteral("/fotos/a.jpg")));
            if (!QFile::exists(right + QStringLiteral("/sobra.txt"))) {
                QVERIFY(dialog.comparison().sameTrees());
            }
            // Queda el estado del par para la próxima vez en los dos sentidos.
            QVERIFY(QFile::exists(sync::stateFile(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
                                                  left, right)));

            // Se puede volver a comparar. Lo borrado a un lado se reconoce como
            // borrado (no como «falta») y se pregunta qué hacer con ello.
            QVERIFY(QFile::remove(right + QStringLiteral("/nuevo.txt")));
            dialog.startCompare();
            QTRY_VERIFY(!dialog.isBusy());
            QVERIFY2(dialog.summary()->text().contains(QStringLiteral("borrado")), qPrintable(dialog.summary()->text()));
            QStringList states;
            for (int row = 0; row < dialog.view()->model()->rowCount(); ++row) {
                states.append(dialog.view()->model()->index(row, 0).data().toString());
            }
            QVERIFY2(states.contains(QStringLiteral("Borrado a la derecha")), qPrintable(states.join(QLatin1Char(','))));
            dialog.setAskAboutDeletions();
            QList<sync::Action> asked;
            dialog.setDeletionChooserForTesting([&](const QList<sync::Action> &deletions) {
                asked = deletions;
                QHash<QString, sync::DeletionChoice> choices;
                for (const sync::Action &action : deletions) {
                    choices.insert(action.relativePath, sync::DeletionChoice::Propagate);
                }
                return std::optional<QHash<QString, sync::DeletionChoice>>(choices);
            });
            dialog.simulate();
            QCOMPARE(asked.size(), 1);
            QCOMPARE(asked.first().relativePath, QStringLiteral("nuevo.txt"));
            QVERIFY(!asked.first().deletedOnLeft);
            QVERIFY(dialog.planIsCurrent());
            bool trashesLeft = false;
            for (const sync::Action &action : dialog.plan().actions) {
                trashesLeft = trashesLeft
                    || (action.relativePath == QStringLiteral("nuevo.txt") && action.kind == sync::ActionKind::TrashLeft);
            }
            QVERIFY(trashesLeft);
            // Si se cancela la pregunta, no queda simulación.
            dialog.setDeletionChooserForTesting([](const QList<sync::Action> &) {
                return std::optional<QHash<QString, sync::DeletionChoice>>();
            });
            dialog.startCompare();
            QTRY_VERIFY(!dialog.isBusy());
            dialog.simulate();
            QVERIFY(!dialog.planIsCurrent());
        });
        window.showSync();
        QVERIFY(driven);
        QTRY_VERIFY(!window.fileActions()->isBusy());
        window.setDualPane(false);
    }

    void tabsPinLockReopenAndFullSession()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        for (const QString &name : {QStringLiteral("a/dentro"), QStringLiteral("b"), QStringLiteral("c")}) {
            QVERIFY(QDir(root).mkpath(name));
        }
        {
            MainWindow window(m_scheduler, openJournal(data));
            BrowserPane *a = window.openTab(root + QStringLiteral("/a"));
            BrowserPane *b = window.openTab(root + QStringLiteral("/b"));
            BrowserPane *c = window.openTab(root + QStringLiteral("/c"));
            QVERIFY(waitReady(a) && waitReady(b) && waitReady(c));
            window.show();

            // Fijar: va delante y la ✕ no la cierra.
            window.setTabPinned(c, true);
            QCOMPARE(window.allPanes().first(), c);
            window.closeTab(0);
            QCOMPARE(window.tabCount(), 3);

            // Bloquear: ir a otro sitio desde ella abre otra pestaña.
            window.setTabLocked(a, true);
            a->navigateTo(root + QStringLiteral("/a/dentro"));
            QCOMPARE(a->currentPath(), root + QStringLiteral("/a"));
            QCOMPARE(window.tabCount(), 4);
            QCOMPARE(window.currentPane()->currentPath(), root + QStringLiteral("/a/dentro"));
            a->goUp();
            QCOMPARE(a->currentPath(), root + QStringLiteral("/a"));
            QCOMPARE(window.tabCount(), 5);
            QCOMPARE(window.currentPane()->currentPath(), root);
            QVERIFY(waitReady(window.currentPane()));
            QTRY_COMPARE(window.currentPane()->selectedPaths(), (QStringList{root + QStringLiteral("/a")}));

            // Cerrar y reabrir: vuelve con su carpeta.
            const int before = window.tabCount();
            window.closeTab(window.tabCount() - 1);
            QCOMPARE(window.tabCount(), before - 1);
            QCOMPARE(window.closedTabCount(), 1);
            window.reopenClosedTab();
            QCOMPARE(window.tabCount(), before);
            QCOMPARE(window.currentPane()->currentPath(), root);
            QCOMPARE(window.closedTabCount(), 0);

            // Cada pestaña con su vista; al cerrar se guarda todo.
            b->setViewMode(BrowserPane::ViewMode::Columns);
            QVERIFY(window.close());
        }
        MainWindow restored(m_scheduler, openJournal(data));
        restored.restoreSession();
        const QList<BrowserPane *> panes = restored.allPanes();
        QCOMPARE(panes.size(), 5);
        QCOMPARE(panes.at(0)->currentPath(), root + QStringLiteral("/c"));
        QVERIFY(panes.at(0)->isPinned());
        QCOMPARE(panes.at(1)->currentPath(), root + QStringLiteral("/a"));
        QVERIFY(panes.at(1)->isLocked());
        QCOMPARE(panes.at(2)->currentPath(), root + QStringLiteral("/b"));
        QCOMPARE(panes.at(2)->viewMode(), BrowserPane::ViewMode::Columns);
        QVERIFY(!panes.at(2)->isPinned() && !panes.at(2)->isLocked());
        // Para las demás pruebas: una sesión sencilla.
        for (BrowserPane *pane : panes) {
            pane->setPinned(false);
            pane->setLocked(false);
            pane->setViewMode(BrowserPane::ViewMode::Details);
        }
        restored.show();
        QVERIFY(restored.close());
    }

    void preferencesThemeShortcutsAndExport()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        MainWindow window(m_scheduler, openJournal(data));
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        const QList<QAction *> actions = window.customizableActions();
        const auto actionNamed = [&actions](const QString &id) -> QAction * {
            for (QAction *action : actions) {
                if (shortcutId(action) == id) {
                    return action;
                }
            }
            return nullptr;
        };
        QAction *copy = actionNamed(QStringLiteral("copiar"));
        QAction *cut = actionNamed(QStringLiteral("cortar"));
        QVERIFY(copy != nullptr && cut != nullptr);
        QCOMPARE(copy->shortcut(), QKeySequence(QKeySequence::Copy));
        const QColor systemBase = QApplication::palette().color(QPalette::Base);
        const QString exported = data.filePath(QStringLiteral("config.ini"));

        bool driven = false;
        window.setPreferencesDriverForTesting([&](PreferencesDialog &dialog) {
            driven = true;
            // Tema oscuro e iconos grandes: al momento y guardado.
            Appearance dark;
            dark.theme = ThemeKind::Dark;
            dark.listIconSize = 24;
            dialog.chooseAppearance(dark);
            QVERIFY(QApplication::palette().color(QPalette::Base).lightnessF() < 0.3f);
            QCOMPARE(QApplication::style()->name().toLower(), QStringLiteral("fusion"));
            QCOMPARE(pane->view()->iconSize(), QSize(24, 24));
            QCOMPARE(QSettings().value(QStringLiteral("apariencia/tema")).toString(), QStringLiteral("oscuro"));
            // Personalizado: el texto de la selección se elige legible.
            Appearance custom;
            custom.theme = ThemeKind::Custom;
            custom.selection = QColor(Qt::yellow);
            dialog.chooseAppearance(custom);
            QCOMPARE(QApplication::palette().color(QPalette::Highlight), QColor(Qt::yellow));
            QCOMPARE(QApplication::palette().color(QPalette::HighlightedText), QColor(Qt::black));

            // Atajos: libre, ocupado (con y sin quitárselo al otro) y reservado.
            QVERIFY(dialog.assignShortcut(copy, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Y)));
            QCOMPARE(copy->shortcut(), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Y));
            QVERIFY(QSettings().contains(QStringLiteral("atajos/copiar")));
            dialog.setConflictAnswerForTesting([](QAction *) { return false; });
            QVERIFY(!dialog.assignShortcut(copy, QKeySequence(QKeySequence::Cut)));
            QVERIFY(dialog.shortcutMessage()->text().contains(QStringLiteral("cortar")));
            dialog.setConflictAnswerForTesting([](QAction *) { return true; });
            QVERIFY(dialog.assignShortcut(copy, QKeySequence(QKeySequence::Cut)));
            QCOMPARE(copy->shortcut(), QKeySequence(QKeySequence::Cut));
            QVERIFY(cut->shortcuts().isEmpty());
            QVERIFY(!dialog.assignShortcut(copy, QKeySequence(Qt::ALT | Qt::Key_F4)));

            // Exportar, volver a la de fábrica e importar.
            QVERIFY(dialog.exportTo(exported));
            QVERIFY(QSettings(exported, QSettings::IniFormat).contains(QStringLiteral("atajos/copiar")));
            QVERIFY(!QSettings(exported, QSettings::IniFormat).contains(QStringLiteral("session/tabs")));
            dialog.setConfirmForTesting([] { return true; });
            dialog.restoreFactorySettings();
            QCOMPARE(copy->shortcut(), QKeySequence(QKeySequence::Copy));
            QCOMPARE(cut->shortcut(), QKeySequence(QKeySequence::Cut));
            QCOMPARE(QApplication::palette().color(QPalette::Base), systemBase);
            QCOMPARE(pane->view()->iconSize(), QSize(16, 16));
            QVERIFY(dialog.importFrom(exported));
            QCOMPARE(copy->shortcut(), QKeySequence(QKeySequence::Cut));
            QVERIFY(cut->shortcuts().isEmpty());
            QCOMPARE(QApplication::palette().color(QPalette::Highlight), QColor(Qt::yellow));
            QVERIFY(!dialog.importFrom(directory.filePath(QStringLiteral("no-es-config.ini"))));

            // Para las demás pruebas.
            dialog.restoreFactorySettings();
        });
        window.showPreferences();
        QVERIFY(driven);
        QCOMPARE(copy->shortcut(), QKeySequence(QKeySequence::Copy));
    }

    void printingImagesPdfTextAndDocuments()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        const auto pagesOf = [](const QString &pdf) {
            return QImageReader::supportedImageFormats().contains("pdf") ? QImageReader(pdf, "pdf").imageCount() : -1;
        };
        const auto printTo = [&](const QString &path, PrintKind kind, int from, int to) {
            const QString out = directory.filePath(QFileInfo(path).fileName() + QStringLiteral(".impreso.pdf"));
            QPrinter printer(QPrinter::HighResolution);
            printer.setOutputFormat(QPrinter::PdfFormat);
            printer.setOutputFileName(out);
            if (from > 0) {
                printer.setFromTo(from, to);
            }
            QString error;
            const bool ok = printFile(path, kind, &printer, &error);
            return ok && QFileInfo(out).size() > 0 ? out : QStringLiteral("ERROR: ") + error;
        };

        // Texto largo: varias páginas, en el orden del archivo.
        QString lines;
        for (int i = 1; i <= 400; ++i) {
            lines += QStringLiteral("Línea %1 del texto de prueba\n").arg(i);
        }
        const QString text = directory.filePath(QStringLiteral("largo.txt"));
        {
            QFile file(text);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(lines.toUtf8());
        }
        QCOMPARE(printKindFor(text), PrintKind::Text);
        QString printed = printTo(text, PrintKind::Text, 0, 0);
        QVERIFY2(!printed.startsWith(QStringLiteral("ERROR")), qPrintable(printed));
        if (pagesOf(printed) >= 0) {
            QVERIFY(pagesOf(printed) > 1);
        }

        // Imagen: una página.
        QImage image(640, 480, QImage::Format_RGB32);
        image.fill(Qt::darkCyan);
        const QString photo = directory.filePath(QStringLiteral("foto.png"));
        QVERIFY(image.save(photo));
        QCOMPARE(printKindFor(photo), PrintKind::Image);
        printed = printTo(photo, PrintKind::Image, 0, 0);
        QVERIFY2(!printed.startsWith(QStringLiteral("ERROR")), qPrintable(printed));

        // PDF de tres páginas: solo la 2 y la 3.
        const QString pdf = directory.filePath(QStringLiteral("tres.pdf"));
        {
            QPdfWriter writer(pdf);
            QPainter painter(&writer);
            for (int page = 1; page <= 3; ++page) {
                if (page > 1) {
                    writer.newPage();
                }
                painter.drawText(QPointF(200, 200), QStringLiteral("Página %1").arg(page));
            }
        }
        if (printKindFor(pdf) == PrintKind::Pdf) {
            printed = printTo(pdf, PrintKind::Pdf, 2, 3);
            QVERIFY2(!printed.startsWith(QStringLiteral("ERROR")), qPrintable(printed));
            QCOMPARE(pagesOf(printed), 2);
        }

#ifdef EU_HAVE_LIBARCHIVE
        // Un Word, como su vista previa.
        const QString word = directory.filePath(QStringLiteral("carta.docx"));
        QVERIFY(eu::testsupport::writeZip(word, {{QStringLiteral("word/document.xml"),
                                                  "<?xml version=\"1.0\"?><w:document xmlns:w=\"w\"><w:body><w:p><w:r>"
                                                  "<w:t>Hola</w:t></w:r></w:p></w:body></w:document>"}}));
        QCOMPARE(printKindFor(word), PrintKind::Document);
        printed = printTo(word, PrintKind::Document, 0, 0);
        QVERIFY2(!printed.startsWith(QStringLiteral("ERROR")), qPrintable(printed));
#endif

        // Lo que no se sabe imprimir; los programas, con aviso y sin abrirlos.
        writeFile(directory.filePath(QStringLiteral("instalar.exe")), 10);
        writeFile(directory.filePath(QStringLiteral("datos.bin")), 10);
        QCOMPARE(printKindFor(directory.filePath(QStringLiteral("instalar.exe"))), PrintKind::None);
        QVERIFY(isPotentiallyDangerous(directory.filePath(QStringLiteral("instalar.exe"))));
        QVERIFY(!isPotentiallyDangerous(photo));

        // Ctrl+P en la ventana imprime lo que tiene el cursor.
        MainWindow window(m_scheduler, openJournal(data));
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        QString asked;
        int askedKind = -1;
        window.setPrintDriverForTesting([&](const QString &path, int kind) {
            asked = path;
            askedKind = kind;
        });
        selectNames(pane, {QStringLiteral("foto.png")});
        window.printCurrent();
        QCOMPARE(asked, photo);
        QCOMPARE(askedKind, int(PrintKind::Image));
    }

    void nameColumnKeepsAUsableWidth()
    {
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("un nombre de archivo bastante largo.txt")));
        MainWindow window(m_scheduler, openJournal(directory));
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        window.resize(1400, 800);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QHeaderView *header = pane->view()->header();
        // Ancha: el nombre se queda con lo que sobra, sin barra horizontal.
        QTRY_VERIFY(header->sectionSize(0) > 300);
        QTRY_COMPARE(header->length(), pane->view()->viewport()->width());
        // Sin sitio (ventana estrecha o columnas anchas): el nombre no baja de
        // un ancho legible y aparece la barra horizontal. (La ventana no se
        // estrecha en la prueba: su ancho mínimo cambia con el sistema.)
        const int typeColumn = 2;
        const int typeWidth = header->sectionSize(typeColumn);
        header->resizeSection(typeColumn, 3000);
        QCOMPARE(header->sectionSize(0), 220);
        header->resizeSection(typeColumn, typeWidth);
        QCOMPARE(header->length(), pane->view()->viewport()->width());
    }

    void treeArrowsPasteAndRefresh()
    {
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("raiz/alfa/dentro")));
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("raiz/beta")));
        const QString root = QDir::cleanPath(directory.filePath(QStringLiteral("raiz")));
        const QString alfa = root + QStringLiteral("/alfa");
        const QString beta = root + QStringLiteral("/beta");
        writeFile(root + QStringLiteral("/nota.txt"), 3);

        MainWindow window(m_scheduler, openJournal(directory));
        BrowserPane *pane = window.openTab(root);
        QVERIFY(waitReady(pane));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.activateWindow();

        Sidebar *sidebar = window.sidebar();
        sidebar->addPlace(QStringLiteral("Raíz"), root);
        const auto indexOf = [sidebar](const QString &path) {
            const QPoint position = sidebar->positionOf(path);
            return position.isNull() ? QModelIndex() : sidebar->indexAt(position);
        };
        QTest::mouseClick(sidebar->viewport(), Qt::LeftButton, Qt::NoModifier, sidebar->positionOf(root));
        QTRY_VERIFY(indexOf(beta).isValid());
        // Flecha solo en las carpetas que tienen subcarpetas.
        QTRY_VERIFY(sidebar->model()->hasChildren(indexOf(alfa)));
        QVERIFY(!sidebar->model()->hasChildren(indexOf(beta)));

        // Ctrl+V con el teclado en el árbol pega en la carpeta elegida en él.
        std::unique_ptr<QMimeData> copied(makeFileMimeData({root + QStringLiteral("/nota.txt")}, false));
        QGuiApplication::clipboard()->setMimeData(copied.release());
        QTest::mouseClick(sidebar->viewport(), Qt::LeftButton, Qt::NoModifier, sidebar->positionOf(beta));
        QTRY_COMPARE(pane->currentPath(), beta);
        QCOMPARE(window.focusWidget(), sidebar);
        QCOMPARE(sidebar->currentPlace(), beta);
        QTest::keyClick(sidebar, Qt::Key_V, Qt::ControlModifier);
        QVERIFY(waitIdle(window));
        QTRY_VERIFY(QFileInfo(beta + QStringLiteral("/nota.txt")).isFile());

        // Una carpeta pegada en "beta": el árbol le pone flecha y la muestra.
        std::unique_ptr<QMimeData> folder(makeFileMimeData({alfa}, false));
        QGuiApplication::clipboard()->setMimeData(folder.release());
        QTest::keyClick(sidebar, Qt::Key_V, Qt::ControlModifier);
        QVERIFY(waitIdle(window));
        QTRY_VERIFY(sidebar->model()->hasChildren(indexOf(beta)));
        sidebar->expand(indexOf(beta));
        QTRY_VERIFY(indexOf(beta + QStringLiteral("/alfa")).isValid());
        QTRY_VERIFY(sidebar->model()->hasChildren(indexOf(beta + QStringLiteral("/alfa"))));

        // Hecho por otro programa: al leer la carpeta, el árbol se pone al día.
        QVERIFY(QDir(root).mkdir(QStringLiteral("gamma")));
        QVERIFY(QDir(beta + QStringLiteral("/alfa")).removeRecursively());
        pane->navigateTo(root);
        QVERIFY(waitReady(pane));
        QTRY_VERIFY(indexOf(root + QStringLiteral("/gamma")).isValid());
        QVERIFY(!sidebar->model()->hasChildren(indexOf(root + QStringLiteral("/gamma"))));
        pane->navigateTo(beta);
        QVERIFY(waitReady(pane));
        QTRY_VERIFY(!sidebar->model()->hasChildren(indexOf(beta)));
    }

    void treeCopiesCutsAndDeletesItsSubfolders()
    {
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("raiz/alfa/dentro")));
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("raiz/beta")));
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("otra")));
        const QString root = QDir::cleanPath(directory.filePath(QStringLiteral("raiz")));
        const QString alfa = root + QStringLiteral("/alfa");
        const QString beta = root + QStringLiteral("/beta");

        MainWindow window(m_scheduler, openJournal(directory));
        BrowserPane *pane = window.openTab(directory.filePath(QStringLiteral("otra")));
        QVERIFY(waitReady(pane));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        window.activateWindow();
        QStringList confirmed;
        window.setDeleteConfirmation([&](const QStringList &paths) {
            confirmed = paths;
            return true;
        });

        Sidebar *sidebar = window.sidebar();
        sidebar->addPlace(QStringLiteral("Raíz"), root);
        QTest::mouseClick(sidebar->viewport(), Qt::LeftButton, Qt::NoModifier, sidebar->positionOf(root));
        QTRY_COMPARE(pane->currentPath(), root);
        QTRY_VERIFY(!sidebar->positionOf(alfa).isNull());

        // Un favorito (primer nivel) no se borra ni se corta desde el árbol.
        QCOMPARE(sidebar->currentPlace(), root);
        QVERIFY(sidebar->currentSubfolder().isEmpty());
        QTest::keyClick(sidebar, Qt::Key_Delete, Qt::ShiftModifier);
        QVERIFY(confirmed.isEmpty());
        QVERIFY(QFileInfo(root).isDir());

        // Ctrl+C copia la carpeta elegida en el árbol, no lo seleccionado en la lista.
        QTest::mouseClick(sidebar->viewport(), Qt::LeftButton, Qt::NoModifier, sidebar->positionOf(alfa));
        QTRY_COMPARE(pane->currentPath(), alfa);
        QCOMPARE(window.focusWidget(), sidebar);
        QCOMPARE(sidebar->currentSubfolder(), alfa);
        QTest::keyClick(sidebar, Qt::Key_C, Qt::ControlModifier);
        FileSelection copied = fileSelectionFrom(QGuiApplication::clipboard()->mimeData());
        QCOMPARE(copied.paths, QStringList{alfa});
        QVERIFY(!copied.cut);
        QTest::keyClick(sidebar, Qt::Key_X, Qt::ControlModifier);
        QVERIFY(fileSelectionFrom(QGuiApplication::clipboard()->mimeData()).cut);
        QGuiApplication::clipboard()->clear();

        // Mayús+Supr la elimina (tras confirmar); la pestaña que estaba dentro
        // vuelve a la carpeta madre.
        QTest::keyClick(sidebar, Qt::Key_Delete, Qt::ShiftModifier);
        QCOMPARE(confirmed, QStringList{alfa});
        QCOMPARE(pane->currentPath(), root);
        QVERIFY(waitIdle(window));
        QTRY_VERIFY(!QFileInfo::exists(alfa));
        QVERIFY(QFileInfo(beta).isDir());
    }

    void statusBarWeighsSelectedFilesAndFolders()
    {
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        QVERIFY(QDir(root).mkpath(QStringLiteral("carpeta/sub")));
        writeFile(root + QStringLiteral("/carpeta/a.bin"), 3000);
        writeFile(root + QStringLiteral("/carpeta/sub/b.bin"), 2000);
        writeFile(root + QStringLiteral("/suelto.bin"), 1000);

        QTemporaryDir data;
        MainWindow window(m_scheduler, openJournal(data));
        BrowserPane *pane = window.openTab(root);
        QVERIFY(waitReady(pane));
        pane->selectAll();
        // Archivos y carpetas juntos: 1000 + 3000 + 2000 bytes.
        const QString total = QStringLiteral("(%1)").arg(core::formatSize(6000, pane->model()->sizeUnits()));
        QTRY_VERIFY2(pane->statusText().contains(total), qPrintable(pane->statusText()));
        QVERIFY(pane->statusText().contains(QStringLiteral("2 seleccionado")));
        // Lo mismo desde las miniaturas (comparten la selección).
        pane->setViewMode(BrowserPane::ViewMode::Thumbnails);
        QVERIFY(pane->statusText().contains(total));
        pane->setViewMode(BrowserPane::ViewMode::Details);
    }

    void folderCanBeSortedFromTheMenu()
    {
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        writeFile(root + QStringLiteral("/a.txt"), 300);
        writeFile(root + QStringLiteral("/b.txt"), 100);
        writeFile(root + QStringLiteral("/c.txt"), 200);

        QTemporaryDir data;
        MainWindow window(m_scheduler, openJournal(data));
        BrowserPane *pane = window.openTab(root);
        QVERIFY(waitReady(pane));
        const auto names = [pane] {
            QStringList list;
            for (int row = 0; row < pane->model()->rowCount(); ++row) {
                list.append(pane->model()->entryAt(row).name);
            }
            return list;
        };
        QCOMPARE(names(), (QStringList{QStringLiteral("a.txt"), QStringLiteral("b.txt"), QStringLiteral("c.txt")}));
        pane->sortBy(Column::Size, Qt::DescendingOrder);
        QCOMPARE(pane->sortColumn(), Column::Size);
        QCOMPARE(pane->sortOrder(), Qt::DescendingOrder);
        QCOMPARE(names(), (QStringList{QStringLiteral("a.txt"), QStringLiteral("c.txt"), QStringLiteral("b.txt")}));
        pane->sortBy(Column::Size, Qt::AscendingOrder);
        QCOMPARE(names(), (QStringList{QStringLiteral("b.txt"), QStringLiteral("c.txt"), QStringLiteral("a.txt")}));
        pane->sortBy(Column::Name, Qt::DescendingOrder);
        QCOMPARE(names(), (QStringList{QStringLiteral("c.txt"), QStringLiteral("b.txt"), QStringLiteral("a.txt")}));
    }

    void arrowKeysWorkInsideTheTree()
    {
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("raiz/alfa/dentro")));
        QVERIFY(QDir(directory.path()).mkpath(QStringLiteral("raiz/beta")));
        const QString root = QDir::cleanPath(directory.filePath(QStringLiteral("raiz")));

        MainWindow window(m_scheduler, openJournal(directory));
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        Sidebar *sidebar = window.sidebar();
        sidebar->addPlace(QStringLiteral("Raíz"), root);
        const QPoint rootPosition = sidebar->positionOf(root);
        QVERIFY(!rootPosition.isNull());

        // Un clic en el árbol abre la carpeta y deja el teclado en el árbol.
        QTest::mouseClick(sidebar->viewport(), Qt::LeftButton, Qt::NoModifier, rootPosition);
        QTRY_COMPARE(pane->currentPath(), root);
        QCOMPARE(window.focusWidget(), sidebar);
        QCOMPARE(sidebar->currentPlace(), root);

        // El clic ya desplegó la rama; Izquierda la recoge y Derecha la despliega.
        const QModelIndex rootIndex = sidebar->currentIndex();
        QTRY_VERIFY(!sidebar->positionOf(root + QStringLiteral("/alfa")).isNull());
        QVERIFY(sidebar->isExpanded(rootIndex));
        QTest::keyClick(sidebar, Qt::Key_Left);
        QVERIFY(!sidebar->isExpanded(rootIndex));
        QCOMPARE(sidebar->currentPlace(), root);
        QTest::keyClick(sidebar, Qt::Key_Right);
        QVERIFY(sidebar->isExpanded(rootIndex));

        // Derecha otra vez baja a la primera subcarpeta, y la lista la sigue.
        QTest::keyClick(sidebar, Qt::Key_Right);
        QCOMPARE(sidebar->currentPlace(), root + QStringLiteral("/alfa"));
        QTRY_COMPARE(pane->currentPath(), root + QStringLiteral("/alfa"));
        QCOMPARE(window.focusWidget(), sidebar);

        // Abajo y arriba se mueven por el árbol, no por la lista de archivos.
        QTest::keyClick(sidebar, Qt::Key_Down);
        QCOMPARE(sidebar->currentPlace(), root + QStringLiteral("/beta"));
        QTRY_COMPARE(pane->currentPath(), root + QStringLiteral("/beta"));
        QTest::keyClick(sidebar, Qt::Key_Up);
        QCOMPARE(sidebar->currentPlace(), root + QStringLiteral("/alfa"));

        // Izquierda en una rama recogida sube a la carpeta padre.
        QTest::keyClick(sidebar, Qt::Key_Left);
        QCOMPARE(sidebar->currentPlace(), root);
        QTRY_COMPARE(pane->currentPath(), root);
        QCOMPARE(window.focusWidget(), sidebar);
    }

    void searchFindsInSubfoldersAsALocation()
    {
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        QVERIFY(QDir(root).mkpath(QStringLiteral("fotos/2025")));
        QVERIFY(QDir(root).mkpath(QStringLiteral("playa de documentos")));
        writeFile(root + QStringLiteral("/fotos/playa.jpg"), 3000);
        writeFile(root + QStringLiteral("/fotos/2025/playa 2.jpg"), 5000);
        writeFile(root + QStringLiteral("/fotos/2025/monte.jpg"), 100);
        writeFile(root + QStringLiteral("/informe.pdf"), 10);

        MainWindow window(m_scheduler, openJournal(directory));
        BrowserPane *pane = window.openTab(root);
        QVERIFY(waitReady(pane));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto waitSearch = [pane] {
            return QTest::qWaitFor([pane] { return pane->searchState() == BrowserPane::SearchState::Finished; }, 20000);
        };

        // Una consulta mal escrita no saca de la carpeta.
        QVERIFY(window.searchEdit()->isEnabled());
        window.searchEdit()->setText(QStringLiteral("(playa"));
        QTest::keyClick(window.searchEdit(), Qt::Key_Return);
        QVERIFY(pane->showsFolder());

        // Escribir y pulsar Intro: los resultados ocupan la pestaña.
        window.searchEdit()->setText(QStringLiteral("playa"));
        QTest::keyClick(window.searchEdit(), Qt::Key_Return);
        QVERIFY(pane->showsSearch());
        QCOMPARE(searchRootOf(pane->currentPath()), root);
        QCOMPARE(searchQueryOf(pane->currentPath()), QStringLiteral("playa"));
        QVERIFY(waitSearch());
        SearchResultsModel *results = pane->resultsModel();
        QCOMPARE(results->count(), 3);
        QVERIFY(pane->resultsView()->isVisible());
        QVERIFY(!pane->view()->isVisible());
        // Primero lo más cercano: la carpeta que está en la raíz.
        QCOMPARE(results->hitAt(0).entry.name, QStringLiteral("playa de documentos"));
        QVERIFY(results->rowOfPath(root + QStringLiteral("/fotos/2025/playa 2.jpg")) >= 0);
        QCOMPARE(pane->searchProgress().foldersScanned, 4);
        QVERIFY(pane->searchInfo().contains(QStringLiteral("playa")));
        QVERIFY(pane->statusText().contains(QStringLiteral("3 resultado")));

        // La columna Carpeta es relativa a donde se buscó; todo tiene icono.
        const int deep = results->rowOfPath(root + QStringLiteral("/fotos/2025/playa 2.jpg"));
        QCOMPARE(results->index(deep, SearchResultsModel::FolderColumn).data().toString(),
                 QDir::toNativeSeparators(QStringLiteral("fotos/2025")));
        for (int row = 0; row < results->count(); ++row) {
            QVERIFY(!results->index(row, 0).data(Qt::DecorationRole).value<QIcon>().isNull());
        }

        // La ruta enseña la carpeta y, al final, la búsqueda; la pestaña también.
        const auto crumbs = PathBar::crumbsFor(pane->currentPath());
        QVERIFY(crumbs.last().first.contains(QStringLiteral("playa")));
        QCOMPARE(crumbs[crumbs.size() - 2].second, root);

        // Ordenar por tamaño (en segundo plano): el mayor arriba.
        pane->resultsView()->sortByColumn(SearchResultsModel::SizeColumn, Qt::DescendingOrder);
        QTRY_COMPARE(results->hitAt(0).entry.name, QStringLiteral("playa 2.jpg"));

        // En los resultados no se crea ni se pega, pero sí se copia y se borra.
        window.newFolder();
        QVERIFY(!window.fileActions()->isBusy());
        const int victim = results->rowOfPath(root + QStringLiteral("/fotos/playa.jpg"));
        pane->resultsView()->selectionModel()->select(
            results->index(victim, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QCOMPARE(pane->selectedPaths(), QStringList{root + QStringLiteral("/fotos/playa.jpg")});
        QCOMPARE(pane->selectedCount(), 1);
        // Donde la unidad no tiene papelera (carpetas temporales de macOS) se
        // ofrece eliminar para siempre: se acepta, para no dejar un diálogo abierto.
        window.setDeleteConfirmation([](const QStringList &) { return true; });
        window.trashSelection();
        QVERIFY(waitIdle(window));
        QVERIFY(!QFileInfo::exists(root + QStringLiteral("/fotos/playa.jpg")));
        // Lo que ya no existe desaparece de la lista sin volver a buscar.
        QTRY_COMPARE(results->count(), 2);
        QCOMPARE(results->rowOfPath(root + QStringLiteral("/fotos/playa.jpg")), -1);

        // "Abrir la carpeta que lo contiene" lleva a la carpeta con el archivo seleccionado.
        const int other = results->rowOfPath(root + QStringLiteral("/fotos/2025/playa 2.jpg"));
        pane->resultsView()->selectionModel()->select(
            results->index(other, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        pane->openContainingFolder();
        QCOMPARE(pane->currentPath(), root + QStringLiteral("/fotos/2025"));
        QVERIFY(pane->showsFolder());
        QVERIFY(waitReady(pane));
        QTRY_COMPARE(pane->selectedPaths(), QStringList{root + QStringLiteral("/fotos/2025/playa 2.jpg")});
        QCOMPARE(pane->searchState(), BrowserPane::SearchState::Idle);
        QCOMPARE(results->count(), 0);

        // Atrás vuelve a los resultados (se busca de nuevo); Subir, a la carpeta.
        pane->goBack();
        QVERIFY(pane->showsSearch());
        QVERIFY(waitSearch());
        QCOMPARE(results->count(), 2);
        QVERIFY(pane->canGoUp());
        pane->goUp();
        QCOMPARE(pane->currentPath(), root);
        QVERIFY(waitReady(pane));

        // Los filtros valen igual que en el filtro local.
        pane->search(QStringLiteral("ext:jpg size:<1kB"));
        QVERIFY(waitSearch());
        QCOMPARE(results->count(), 1);
        QCOMPARE(results->hitAt(0).entry.name, QStringLiteral("monte.jpg"));
        // Abrir una carpeta encontrada entra en ella.
        pane->search(QStringLiteral("type:folder 2025"));
        QVERIFY(waitSearch());
        QCOMPARE(results->count(), 1);
        emit pane->resultsView()->activated(results->index(0, 0));
        QCOMPARE(pane->currentPath(), root + QStringLiteral("/fotos/2025"));
        QVERIFY(waitReady(pane));

        // Unos resultados no se guardan en la sesión: se guarda la carpeta.
        pane->search(QStringLiteral("playa"));
        QVERIFY(waitSearch());
        QVERIFY(window.close());
        MainWindow restored(m_scheduler);
        restored.restoreSession();
        QCOMPARE(restored.tabCount(), 1);
        QCOMPARE(restored.currentPane()->currentPath(), root + QStringLiteral("/fotos/2025"));
        QSettings().clear();
    }

    void searchScopesAndOptions()
    {
        QTemporaryDir directory;
        const QString base = QDir::cleanPath(directory.path());
        const QString diskA = base + QStringLiteral("/discoA");
        const QString diskB = base + QStringLiteral("/discoB");
        const QString books = diskA + QStringLiteral("/libros");
        QVERIFY(QDir(base).mkpath(QStringLiteral("discoA/libros/WALTER RISO")));
        QVERIFY(QDir(base).mkpath(QStringLiteral("discoB")));
        writeFile(books + QStringLiteral("/WALTER RISO/Amar o depender.pdf"), 10);
        writeFile(diskA + QStringLiteral("/suelto.dat"), 10);
        writeFile(diskB + QStringLiteral("/Riso en B.dat"), 10);
        {
            QFile note(books + QStringLiteral("/apuntes.txt"));
            QVERIFY(note.open(QIODevice::WriteOnly));
            note.write("Una nota sobre Riso y sus libros\n");
        }

        fs::Volume a;
        a.rootPath = diskA;
        a.displayName = QStringLiteral("A");
        fs::Volume b;
        b.rootPath = diskB;
        b.displayName = QStringLiteral("B");

        MainWindow window(m_scheduler, openJournal(directory));
        window.setSearchVolumeSource([a, b] { return QList<fs::Volume>{a, b}; });
        BrowserPane *pane = window.openTab(books);
        QVERIFY(waitReady(pane));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        const auto searchFor = [&](const QString &text) {
            window.searchEdit()->setText(text);
            QTest::keyClick(window.searchEdit(), Qt::Key_Return);
            return pane->showsSearch()
                && QTest::qWaitFor([pane] { return pane->searchState() == BrowserPane::SearchState::Finished; }, 20000);
        };
        SearchResultsModel *results = pane->resultsModel();

        // Lo que falló al dueño: varias palabras con un comodín al final.
        QVERIFY(searchFor(QStringLiteral("walter riso*")));
        QCOMPARE(results->count(), 1);
        QCOMPARE(results->hitAt(0).entry.name, QStringLiteral("WALTER RISO"));

        // Distinguiendo mayúsculas, "walter" ya no es "WALTER".
        window.setSearchCaseSensitive(true);
        QVERIFY(searchFor(QStringLiteral("walter riso*")));
        QCOMPARE(results->count(), 0);
        QVERIFY(pane->searchInfo().contains(QStringLiteral("distingue mayúsculas")));
        QVERIFY(searchFor(QStringLiteral("WALTER RISO*")));
        QCOMPARE(results->count(), 1);
        window.setSearchCaseSensitive(false);

        // Solo la carpeta: el PDF está una carpeta más abajo.
        window.setSearchScope(SearchScope::Folder);
        QVERIFY(searchFor(QStringLiteral("ext:pdf")));
        QCOMPARE(results->count(), 0);
        QCOMPARE(searchRootOf(pane->currentPath()), books);
        window.setSearchScope(SearchScope::Subfolders);
        QVERIFY(searchFor(QStringLiteral("ext:pdf")));
        QCOMPARE(results->count(), 1);

        // Toda la unidad: encuentra lo que está fuera de la carpeta, pero no en la otra unidad.
        window.setSearchScope(SearchScope::Volume);
        QVERIFY(searchFor(QStringLiteral("ext:dat")));
        QCOMPARE(searchSpecOf(pane->currentPath()).roots, QStringList{diskA});
        QCOMPARE(results->count(), 1);
        QCOMPARE(results->filePath(0), diskA + QStringLiteral("/suelto.dat"));

        // Todas las unidades.
        window.setSearchScope(SearchScope::AllVolumes);
        QVERIFY(searchFor(QStringLiteral("ext:dat")));
        QCOMPARE(results->count(), 2);
        QVERIFY(pane->searchInfo().contains(QStringLiteral("todas las unidades")));

        // Las elegidas.
        window.setChosenSearchVolumes({diskB});
        window.setSearchScope(SearchScope::ChosenVolumes);
        QVERIFY(searchFor(QStringLiteral("ext:dat")));
        QCOMPARE(results->count(), 1);
        QCOMPARE(results->filePath(0), diskB + QStringLiteral("/Riso en B.dat"));

        // Subir desde cualquier resultado vuelve a la carpeta de partida.
        pane->goUp();
        QCOMPARE(pane->currentPath(), books);
        QVERIFY(waitReady(pane));
        // Lo que se buscaba sigue escrito al cambiar de carpeta, y las
        // búsquedas hechas quedan en la lista de recientes, sin repetirse.
        QCOMPARE(window.searchEdit()->text(), QStringLiteral("ext:dat"));
        pane->navigateTo(diskA);
        QVERIFY(waitReady(pane));
        QCOMPARE(window.searchEdit()->text(), QStringLiteral("ext:dat"));
        QCOMPARE(window.recentSearches(), (QStringList{QStringLiteral("ext:dat"), QStringLiteral("ext:pdf"),
                                                       QStringLiteral("WALTER RISO*"),
                                                       QStringLiteral("walter riso*")}));
        pane->navigateTo(books);
        QVERIFY(waitReady(pane));

        // En el contenido: la palabra está dentro del archivo, no en su nombre.
        window.setSearchScope(SearchScope::Subfolders);
        window.setSearchInContent(true);
        QVERIFY(searchFor(QStringLiteral("riso libros")));
        QCOMPARE(results->count(), 1);
        QCOMPARE(results->hitAt(0).entry.name, QStringLiteral("apuntes.txt"));
        QVERIFY2(pane->searchInfo().contains(QStringLiteral("documentos leídos: ")), qPrintable(pane->searchInfo()));
        QVERIFY(!pane->searchInfo().contains(QStringLiteral("documentos leídos: 0")));
        // Con índice de contenido de la carpeta, lo mismo sin abrir el archivo.
        window.contentIndexService()->addRoot(books);
        QVERIFY(QTest::qWaitFor([&] {
            const auto all = window.contentIndexService()->status();
            return !all.isEmpty() && all.first().state == index::ContentRootStatus::State::Ready
                && !window.contentIndexService()->isBusy();
        }, 30000));
        QVERIFY(searchFor(QStringLiteral("riso libros")));
        QCOMPARE(results->count(), 1);
        QCOMPARE(results->hitAt(0).entry.name, QStringLiteral("apuntes.txt"));
        QVERIFY2(pane->searchInfo().contains(QStringLiteral("documentos leídos: 0")), qPrintable(pane->searchInfo()));
        QVERIFY(pane->searchInfo().contains(QStringLiteral("del índice de contenido")));
        QVERIFY(!results->hitAt(0).snippet.isEmpty());
        window.contentIndexService()->removeRoot(books);
        window.setSearchInContent(false);
        // El filtro content: hace lo mismo sin cambiar de modo.
        QVERIFY(searchFor(QStringLiteral("content:\"nota sobre riso\"")));
        QCOMPARE(results->count(), 1);

        // Desde la lista de unidades se busca en todas.
        pane->navigateTo(volumesLocation());
        QVERIFY(pane->showsVolumes());
        QVERIFY(window.searchEdit()->isEnabled());
        QVERIFY(searchFor(QStringLiteral("ext:dat")));
        QCOMPARE(results->count(), 2);
        pane->goUp();
        QVERIFY(pane->showsVolumes());
    }

    void searchUsesTheIndexWhenThereIsOne()
    {
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path()) + QStringLiteral("/unidad");
        QVERIFY(QDir().mkpath(root + QStringLiteral("/fotos/2025")));
        writeFile(root + QStringLiteral("/fotos/playa.jpg"), 3000);
        writeFile(root + QStringLiteral("/fotos/2025/playa 2.jpg"), 5000);
        writeFile(root + QStringLiteral("/informe.pdf"), 10);

        MainWindow window(m_scheduler, openJournal(directory));
        BrowserPane *pane = window.openTab(root);
        QVERIFY(waitReady(pane));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        index::IndexService *service = window.indexService();
        const auto searchFor = [&](const QString &text) {
            window.searchEdit()->setText(text);
            QTest::keyClick(window.searchEdit(), Qt::Key_Return);
            return pane->showsSearch()
                && QTest::qWaitFor([pane] { return pane->searchState() == BrowserPane::SearchState::Finished; }, 20000);
        };
        SearchResultsModel *results = pane->resultsModel();
        const auto origin = [&] {
            return results->index(0, SearchResultsModel::OriginColumn).data().toString();
        };

        // Sin índice se recorre el disco.
        QVERIFY(searchFor(QStringLiteral("playa")));
        QCOMPARE(results->count(), 2);
        QVERIFY(!pane->searchFromIndex());
        QCOMPARE(origin(), QStringLiteral("Disco"));
        QVERIFY(pane->searchProgress().foldersScanned > 0);

        // La ventana "Índice de búsqueda" lista las unidades; marcar una la indexa.
        fs::Volume volume;
        volume.rootPath = root;
        volume.displayName = QStringLiteral("Pruebas");
        IndexDialog dialog(*service, [volume] { return QList<fs::Volume>{volume}; });
        QCOMPARE(dialog.list()->topLevelItemCount(), 1);
        QCOMPARE(dialog.list()->topLevelItem(0)->checkState(0), Qt::Unchecked);
        // El servicio de NTFS solo se ofrece en Windows.
#ifdef Q_OS_WIN
        QVERIFY(!dialog.serviceBox()->isHidden());
#else
        QVERIFY(dialog.serviceBox()->isHidden());
#endif
        QSignalSpy rootsChanged(&dialog, &IndexDialog::rootsChanged);
        dialog.setIndexed(root, true);
        QCOMPARE(rootsChanged.count(), 1);
        QCOMPARE(service->roots(), QStringList{root});
        QVERIFY(QTest::qWaitFor([&] {
            const auto status = service->statusOf(root);
            return status && status->state == index::RootStatus::State::Ready && !service->isBusy();
        }, 30000));
        QCOMPARE(dialog.list()->topLevelItem(0)->checkState(0), Qt::Checked);
        QVERIFY(dialog.list()->topLevelItem(0)->text(2).startsWith(QStringLiteral("Al día")));

        // Con índice, la misma búsqueda sale de él y lo dice.
        QVERIFY(searchFor(QStringLiteral("playa")));
        QCOMPARE(results->count(), 2);
        QVERIFY(pane->searchFromIndex());
        QCOMPARE(origin(), QStringLiteral("Índice"));
        QCOMPARE(pane->searchProgress().foldersScanned, 0);
        QVERIFY(pane->searchInfo().contains(QStringLiteral("índice")));
        // También desde una subcarpeta.
        pane->navigateTo(root + QStringLiteral("/fotos/2025"));
        QVERIFY(waitReady(pane));
        QVERIFY(searchFor(QStringLiteral("playa")));
        QCOMPARE(results->count(), 1);
        QVERIFY(pane->searchFromIndex());

        // "Verificar en disco" repite la búsqueda recorriendo las carpetas.
        pane->verifySearchOnDisk();
        QVERIFY(QTest::qWaitFor([pane] { return pane->searchState() == BrowserPane::SearchState::Finished; }, 20000));
        QVERIFY(!pane->searchFromIndex());
        QCOMPARE(results->count(), 1);
        QCOMPARE(origin(), QStringLiteral("Disco"));

        // Lo que el índice no sabe (el contenido) se busca en el disco sin más.
        QVERIFY(searchFor(QStringLiteral("content:x")));
        QVERIFY(!pane->searchFromIndex());

        // Desmarcar la unidad borra su índice.
        dialog.setIndexed(root, false);
        QVERIFY(service->roots().isEmpty());
        QVERIFY(searchFor(QStringLiteral("playa")));
        QVERIFY(!pane->searchFromIndex());
    }

    void thumbnailViewKeepsTheSelectionAndShowsImages()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        for (const QString &name : {QStringLiteral("playa.png"), QStringLiteral("monte.png")}) {
            QImage image(300, 200, QImage::Format_RGB32);
            image.fill(Qt::darkGreen);
            QVERIFY(image.save(directory.filePath(name)));
        }
        writeFile(directory.filePath(QStringLiteral("notas.txt")), 10);
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("carpeta")));

        MainWindow window(m_scheduler, openJournal(data));
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        selectNames(pane, {QStringLiteral("playa.png")});
        QCOMPARE(pane->selectedCount(), 1);

        // Ctrl+2 cambia a miniaturas sin perder la carpeta ni la selección.
        window.setViewMode(BrowserPane::ViewMode::Thumbnails);
        QCOMPARE(pane->viewMode(), BrowserPane::ViewMode::Thumbnails);
        ThumbnailView *thumbnails = pane->thumbnailView();
        QVERIFY(thumbnails->isVisible());
        QVERIFY(!pane->view()->isVisible());
        QCOMPARE(pane->fileView(), static_cast<QAbstractItemView *>(thumbnails));
        QCOMPARE(pane->selectedCount(), 1);
        QCOMPARE(thumbnails->selectionModel()->selectedRows().size(), 1);
        QCOMPARE(thumbnails->model()->rowCount(), 4);

        // Las miniaturas de lo que se ve se generan solas, en segundo plano.
        thumbs::ThumbnailService *service = window.thumbnailService();
        const auto stateOf = [&](const QString &name) {
            const QFileInfo info(directory.filePath(name));
            thumbs::ThumbnailRequest request;
            request.path = QDir::cleanPath(info.filePath());
            request.size = info.size();
            request.modifiedMs = info.lastModified().toMSecsSinceEpoch();
            request.maxSide = 256;   // 160 px lógicos con escala 1, redondeado al escalón
            return service->cache().lookupMemory(request).state;
        };
        QTRY_COMPARE_WITH_TIMEOUT(stateOf(QStringLiteral("playa.png")), thumbs::ThumbnailCache::State::Ready, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(stateOf(QStringLiteral("monte.png")), thumbs::ThumbnailCache::State::Ready, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(service->pendingCount(), 0, 10000);   // también la carpeta y el texto

        // Seleccionar aquí, con el ratón y con Mayús+flecha, cuenta igual que en detalles.
        const int montoRow = pane->model()->rowOfName(QStringLiteral("monte.png"));
        const QRect montoRect = thumbnails->visualRect(pane->model()->index(montoRow, 0));
        QTest::mouseClick(thumbnails->viewport(), Qt::LeftButton, Qt::NoModifier, montoRect.center());
        QCOMPARE(pane->selectedCount(), 1);
        QTest::keyClick(thumbnails, Qt::Key_Right, Qt::ShiftModifier);
        QCOMPARE(pane->selectedCount(), 2);
        selectNames(pane, {QStringLiteral("playa.png")});

        // Cambiar el tamaño no pierde nada.
        window.setThumbnailSize(96);
        QCOMPARE(thumbnails->thumbnailSize(), 96);

        // Renombrar también funciona aquí, con el nombre sin extensión seleccionado.
        window.renameSelection();
        QTRY_VERIFY(thumbnails->findChild<QLineEdit *>() != nullptr);
        auto *editor = thumbnails->findChild<QLineEdit *>();
        QTRY_COMPARE(editor->selectedText(), QStringLiteral("playa"));
        editor->setText(QStringLiteral("costa.png"));
        QTest::keyClick(editor, Qt::Key_Return);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath(QStringLiteral("costa.png"))), 10000);
        QVERIFY(waitIdle(window));

        // De vuelta a detalles (Ctrl+1), con lo renombrado seleccionado.
        window.setViewMode(BrowserPane::ViewMode::Details);
        QVERIFY(pane->view()->isVisible());
        // La lista se vuelve a leer tras renombrar: se espera a que lo haga.
        QTRY_COMPARE_WITH_TIMEOUT(pane->model()->entryAt(pane->view()->currentIndex().row()).name,
                                  QStringLiteral("costa.png"), 10000);
        QCOMPARE(pane->selectedCount(), 1);

        // Una pestaña nueva hereda la vista de la activa.
        window.setViewMode(BrowserPane::ViewMode::Thumbnails);
        BrowserPane *second = window.openTab(directory.path());
        QCOMPARE(second->viewMode(), BrowserPane::ViewMode::Thumbnails);
    }

    void previewPanelShowsDataAndContent()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        QImage image(400, 300, QImage::Format_RGB32);
        image.fill(Qt::darkBlue);
        QVERIFY(image.save(directory.filePath(QStringLiteral("mar.png"))));
        {
            QFile text(directory.filePath(QStringLiteral("notas.txt")));
            QVERIFY(text.open(QIODevice::WriteOnly));
            text.write("Primera línea\nSegunda línea\n");
        }
        {
            QFile markdown(directory.filePath(QStringLiteral("LEEME.md")));
            QVERIFY(markdown.open(QIODevice::WriteOnly));
            markdown.write("# Título grande\n\nUn párrafo con **negrita**.\n");
        }
        writeFile(directory.filePath(QStringLiteral("datos.bin")), 100);
#ifdef EU_HAVE_LIBARCHIVE
        QVERIFY(eu::testsupport::writeZip(directory.filePath(QStringLiteral("carta.docx")), {
            {QStringLiteral("word/document.xml"),
             "<?xml version=\"1.0\"?><w:document xmlns:w=\"w\"><w:body><w:p><w:pPr><w:pStyle w:val=\"Heading1\"/>"
             "</w:pPr><w:r><w:t>Querida familia</w:t></w:r></w:p><w:p><w:r><w:rPr><w:b/></w:rPr><w:t>Les escribo</w:t>"
             "</w:r></w:p></w:body></w:document>"}}));
        QVERIFY(eu::testsupport::writeZip(directory.filePath(QStringLiteral("gastos.xlsx")), {
            {QStringLiteral("xl/workbook.xml"),
             "<?xml version=\"1.0\"?><workbook xmlns:r=\"r\"><sheets><sheet name=\"Enero\" r:id=\"rId1\"/>"
             "<sheet name=\"Febrero\" r:id=\"rId2\"/></sheets></workbook>"},
            {QStringLiteral("xl/_rels/workbook.xml.rels"),
             "<?xml version=\"1.0\"?><Relationships><Relationship Id=\"rId1\" Target=\"worksheets/sheet1.xml\"/>"
             "<Relationship Id=\"rId2\" Target=\"worksheets/sheet2.xml\"/></Relationships>"},
            {QStringLiteral("xl/worksheets/sheet1.xml"),
             "<?xml version=\"1.0\"?><worksheet><sheetData><row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>Luz</t></is>"
             "</c><c r=\"B1\"><v>350</v></c></row></sheetData></worksheet>"},
            {QStringLiteral("xl/worksheets/sheet2.xml"),
             "<?xml version=\"1.0\"?><worksheet><sheetData><row r=\"4\"><c r=\"C4\" t=\"inlineStr\"><is><t>Agua</t></is>"
             "</c></row></sheetData></worksheet>"}}));
#endif

        MainWindow window(m_scheduler, openJournal(data));
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        PreviewPanel *panel = window.previewPanel();
        QVERIFY(panel->isVisible());
        QCOMPARE(panel->shown(), PreviewPanel::Shown::Nothing);   // nada seleccionado aún

        // Una foto: sus datos arriba y la imagen abajo.
        selectNames(pane, {QStringLiteral("mar.png")});
        QCOMPARE(panel->title(), QStringLiteral("mar.png"));
        QTRY_COMPARE_WITH_TIMEOUT(panel->shown(), PreviewPanel::Shown::Picture, 10000);
        QVERIFY(!panel->isLoading());
        QCOMPARE(panel->fieldValue(QStringLiteral("Dimensiones")), QStringLiteral("400 × 300 píxeles"));
        QVERIFY(panel->fieldValue(QStringLiteral("Tamaño")).contains(QStringLiteral("bytes")));
        QCOMPARE(panel->picture().size(), QSize(400, 300));   // no se agranda

        // Un texto se ve como texto; un Markdown, ya formateado.
        selectNames(pane, {QStringLiteral("notas.txt")});
        QTRY_COMPARE_WITH_TIMEOUT(panel->shown(), PreviewPanel::Shown::Text, 10000);
        QVERIFY(panel->text().startsWith(QStringLiteral("Primera línea")));
        selectNames(pane, {QStringLiteral("LEEME.md")});
        QTRY_COMPARE_WITH_TIMEOUT(panel->shown(), PreviewPanel::Shown::Markdown, 10000);
        QVERIFY(panel->text().contains(QStringLiteral("Título grande")));
        QVERIFY(!panel->text().contains(QLatin1Char('#')));

        // Lo que no tiene vista previa lo dice, y enseña igual sus datos.
        selectNames(pane, {QStringLiteral("datos.bin")});
        QTRY_COMPARE_WITH_TIMEOUT(panel->shown(), PreviewPanel::Shown::Message, 10000);
        QTRY_VERIFY(!panel->isLoading());
        QVERIFY(panel->fieldValue(QStringLiteral("Tamaño")).contains(QStringLiteral("100")));

#ifdef EU_HAVE_LIBARCHIVE
        // Word con su formato y Excel como cuadrícula, sin LibreOffice.
        selectNames(pane, {QStringLiteral("carta.docx")});
        QTRY_COMPARE_WITH_TIMEOUT(panel->shown(), PreviewPanel::Shown::Document, 10000);
        QVERIFY2(panel->text().contains(QStringLiteral("Querida familia")), qPrintable(panel->text()));
        QVERIFY(panel->text().contains(QStringLiteral("Les escribo")));
        selectNames(pane, {QStringLiteral("gastos.xlsx")});
        QTRY_COMPARE_WITH_TIMEOUT(panel->shown(), PreviewPanel::Shown::Spreadsheet, 10000);
        QCOMPARE(panel->sheetNames(), (QStringList{QStringLiteral("Enero"), QStringLiteral("Febrero")}));
        QCOMPARE(panel->text(), QStringLiteral("Luz\t350"));
        panel->showSheet(1);
        QCOMPARE(panel->text(), QStringLiteral("\t\tAgua"));
#endif

        // Al recorrer deprisa, solo cuenta el último.
        selectNames(pane, {QStringLiteral("mar.png")});
        selectNames(pane, {QStringLiteral("notas.txt")});
        QTRY_COMPARE_WITH_TIMEOUT(panel->shown(), PreviewPanel::Shown::Text, 10000);
        QCOMPARE(panel->title(), QStringLiteral("notas.txt"));

        // Varios seleccionados: lo dice y muestra el actual.
        selectNames(pane, {QStringLiteral("mar.png"), QStringLiteral("notas.txt")});
        QTRY_VERIFY(panel->fieldValue(QStringLiteral("Selección")).startsWith(QStringLiteral("2 elementos")));

        // Alt+P lo oculta y vuelve a mostrar.
        window.setPreviewVisible(false);
        QVERIFY(!panel->isVisible());
        window.setPreviewVisible(true);
        QVERIFY(panel->isVisible());
    }

    void videosPlayInsideTheirThumbnail()
    {
#ifndef EU_HAVE_FFMPEG
        QSKIP("Sin FFmpeg no se puede fabricar el video de prueba.");
#else
        if (!InlinePlayer::isAvailable()) {
            QSKIP("Compilado sin Qt Multimedia.");
        }
        QTemporaryDir data;
        QTemporaryDir directory;
        QVERIFY(eu::testsupport::writeColorVideo(directory.filePath(QStringLiteral("a-paseo.mov")),
                                                 {{Qt::red, 30}, {Qt::blue, 30}}, 10, QSize(160, 120)));
        QVERIFY(eu::testsupport::writeColorVideo(directory.filePath(QStringLiteral("b-fiesta.mov")),
                                                 {{Qt::green, 40}}, 10, QSize(160, 120)));
        writeFile(directory.filePath(QStringLiteral("c-notas.txt")), 10);

        MainWindow window(m_scheduler, openJournal(data));
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        window.setViewMode(BrowserPane::ViewMode::Thumbnails);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        ThumbnailView *thumbnails = pane->thumbnailView();
        InlinePlayer *player = window.inlinePlayer();
        const auto rectOf = [&](const QString &name) {
            return thumbnails->visualRect(pane->model()->index(pane->model()->rowOfName(name), 0));
        };
        QVERIFY(thumbnails->isPlayableRow(pane->model()->rowOfName(QStringLiteral("a-paseo.mov"))));
        QVERIFY(!thumbnails->isPlayableRow(pane->model()->rowOfName(QStringLiteral("c-notas.txt"))));

        // Clic en ▶: se reproduce dentro de la miniatura, en silencio y sin seleccionarlo.
        const QPoint button = thumbnails->playButtonRect(rectOf(QStringLiteral("a-paseo.mov"))).center();
        QTest::mouseMove(thumbnails->viewport(), button);
        QTest::mouseClick(thumbnails->viewport(), Qt::LeftButton, Qt::NoModifier, button);
        QVERIFY(player->isPlaying());
        QCOMPARE(player->path(), QDir::cleanPath(directory.filePath(QStringLiteral("a-paseo.mov"))));
        QVERIFY(!player->isAudio());
        QCOMPARE(pane->selectedCount(), 0);
        QTRY_VERIFY_WITH_TIMEOUT(!player->frame().isNull() || !player->error().isEmpty(), 10000);
        QVERIFY2(player->error().isEmpty(), qPrintable(player->error()));

        // Otro clic en el botón (ahora ■) lo para.
        QTest::mouseClick(thumbnails->viewport(), Qt::LeftButton, Qt::NoModifier, button);
        QVERIFY(!player->isPlaying());

        // Al sacar el puntero de la miniatura, se para.
        QTest::mouseClick(thumbnails->viewport(), Qt::LeftButton, Qt::NoModifier, button);
        QVERIFY(player->isPlaying());
        QTest::mouseMove(thumbnails->viewport(), rectOf(QStringLiteral("c-notas.txt")).center());
        QTRY_VERIFY(!player->isPlaying());

        // Con el teclado: barra espaciadora sobre el elemento con el cursor.
        thumbnails->setFocus();
        selectNames(pane, {QStringLiteral("b-fiesta.mov")});
        QTest::keyClick(thumbnails, Qt::Key_Space);
        QVERIFY(player->isPlaying());
        QVERIFY(player->path().endsWith(QStringLiteral("b-fiesta.mov")));
        // Mover el ratón no lo para (lo empezó el teclado); mover el cursor, sí.
        QTest::mouseMove(thumbnails->viewport(), rectOf(QStringLiteral("c-notas.txt")).center());
        QVERIFY(player->isPlaying());
        QTest::keyClick(thumbnails, Qt::Key_Right);
        QTRY_VERIFY(!player->isPlaying());

        // Desactivado en el menú Ver: no hay botón ni barra espaciadora que valga.
        pane->setInlinePlaybackEnabled(false);
        selectNames(pane, {QStringLiteral("b-fiesta.mov")});
        QTest::keyClick(thumbnails, Qt::Key_Space);
        QVERIFY(!player->isPlaying());
        pane->setInlinePlaybackEnabled(true);

        // Al cambiar de carpeta, lo que sonaba se para.
        QTest::keyClick(thumbnails, Qt::Key_Space);
        QVERIFY(player->isPlaying());
        pane->navigateTo(data.path());
        QTRY_VERIFY(!player->isPlaying());
#endif
    }

    void thumbnailsAcceptFilesDroppedOnAFolder()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("carpeta")));
        writeFile(directory.filePath(QStringLiteral("nota.txt")), 5);

        MainWindow window(m_scheduler, openJournal(data));
        window.resize(900, 600);
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        window.setViewMode(BrowserPane::ViewMode::Thumbnails);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        ThumbnailView *thumbnails = pane->thumbnailView();
        PreviewPanel *panel = window.previewPanel();
        selectNames(pane, {QStringLiteral("nota.txt")});
        QTRY_COMPARE(panel->title(), QStringLiteral("nota.txt"));

        // Qt entrega lo que se suelta al widget más hondo bajo el puntero que
        // lo acepte; se busca igual aquí. Antes, la zona de las miniaturas no
        // lo aceptaba y le llegaba a la vista, que lo descartaba sin más.
        const QPoint onFolder =
            thumbnails->visualRect(pane->model()->index(pane->model()->rowOfName(QStringLiteral("carpeta")), 0))
                .center();
        QWidget *target = window.childAt(thumbnails->viewport()->mapTo(&window, onFolder));
        while (target != nullptr && !target->acceptDrops()) {
            target = target->parentWidget();
        }
        QCOMPARE(target, thumbnails->viewport());

        std::unique_ptr<QMimeData> mime(makeFileMimeData({directory.filePath(QStringLiteral("nota.txt"))}, false));
        QDragEnterEvent enter(onFolder, Qt::CopyAction | Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &enter);
        QDragMoveEvent move(onFolder, Qt::CopyAction | Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &move);
        QVERIFY(move.isAccepted());
        QCOMPARE(move.dropAction(), Qt::MoveAction);   // en la misma unidad, mover
        QDropEvent drop(onFolder, Qt::CopyAction | Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &drop);
        QVERIFY(drop.isAccepted());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath(QStringLiteral("carpeta/nota.txt"))), 10000);
        QVERIFY(waitIdle(window));
        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("nota.txt"))));

        // La vista previa no se queda con el archivo que ya no está ahí.
        QTRY_COMPARE_WITH_TIMEOUT(panel->shown(), PreviewPanel::Shown::Nothing, 10000);

        // Las columnas, que también son listas de Qt, aceptan igual lo que se suelta.
        window.setViewMode(BrowserPane::ViewMode::Columns);
        QVERIFY(waitReady(pane));
        const QList<MillerColumn *> columns = pane->findChildren<MillerColumn *>();
        QVERIFY(!columns.isEmpty());
        for (const MillerColumn *column : columns) {
            QVERIFY(column->viewport()->acceptDrops());
        }
    }

    void thumbnailsMakeRoomForTheSizeWhenAsked()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("nota.txt")), 10);
        MainWindow window(m_scheduler, openJournal(data));
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        window.setViewMode(BrowserPane::ViewMode::Thumbnails);
        ThumbnailView *thumbnails = pane->thumbnailView();
        const int heightWithoutSize = thumbnails->gridSize().height();

        // Una línea más para el tamaño, y se devuelve al quitarlo.
        pane->setFolderSizesEnabled(true);
        QVERIFY(thumbnails->showsSizes());
        QCOMPARE(thumbnails->gridSize().height(), heightWithoutSize + thumbnails->fontMetrics().height());
        pane->setFolderSizesEnabled(false);
        QVERIFY(!thumbnails->showsSizes());
        QCOMPARE(thumbnails->gridSize().height(), heightWithoutSize);
    }

    void ctrlWheelMakesThumbnailsBiggerOrSmaller()
    {
        QTemporaryDir data;
        QTemporaryDir directory;
        for (int i = 0; i < 40; ++i) {
            writeFile(directory.filePath(QStringLiteral("nota-%1.txt").arg(i, 2, 10, QLatin1Char('0'))), 10);
        }
        MainWindow window(m_scheduler, openJournal(data));
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        window.setViewMode(BrowserPane::ViewMode::Thumbnails);
        window.setThumbnailSize(160);
        window.resize(900, 600);
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        ThumbnailView *thumbnails = pane->thumbnailView();
        QWidget *viewport = thumbnails->viewport();
        const auto wheel = [viewport](int delta, Qt::KeyboardModifiers modifiers) {
            const QPointF where(viewport->width() / 2.0, viewport->height() / 2.0);
            QWheelEvent event(where, viewport->mapToGlobal(where), QPoint(), QPoint(0, delta), Qt::NoButton,
                              modifiers, Qt::NoScrollPhase, false);
            QApplication::sendEvent(viewport, &event);
        };

        // Ctrl + rueda hacia delante: un paso más grandes; hacia atrás, más pequeñas.
        wheel(120, Qt::ControlModifier);
        QCOMPARE(thumbnails->thumbnailSize(), 192);
        QCOMPARE(window.thumbnailSize(), 192);
        wheel(-240, Qt::ControlModifier);
        QCOMPARE(thumbnails->thumbnailSize(), 128);
        // Un panel táctil manda trozos pequeños: cuentan al juntar un paso.
        wheel(60, Qt::ControlModifier);
        QCOMPARE(thumbnails->thumbnailSize(), 128);
        wheel(60, Qt::ControlModifier);
        QCOMPARE(thumbnails->thumbnailSize(), 160);
        // Sin Ctrl, la rueda desplaza y no cambia el tamaño.
        wheel(-120, Qt::NoModifier);
        QCOMPARE(thumbnails->thumbnailSize(), 160);

        // Hay un límite por cada lado.
        for (int i = 0; i < 20; ++i) {
            wheel(120, Qt::ControlModifier);
        }
        QCOMPARE(thumbnails->thumbnailSize(), 512);
        for (int i = 0; i < 20; ++i) {
            wheel(-120, Qt::ControlModifier);
        }
        QCOMPARE(thumbnails->thumbnailSize(), 48);

        // También desde el menú y con Ctrl + más o menos; pasando por los tamaños
        // del menú, que entonces queda marcado.
        window.setThumbnailSize(96);
        QAction *medium = nullptr;
        QAction *larger = nullptr;
        for (QAction *action : menuActions(window.menuBar())) {
            if (action->text() == QStringLiteral("Medianas")) {
                medium = action;
            } else if (action->text() == QStringLiteral("Miniaturas más grandes")) {
                larger = action;
            }
        }
        QVERIFY(medium != nullptr && larger != nullptr);
        QCOMPARE(larger->shortcut(), QKeySequence(QKeySequence::ZoomIn));
        larger->trigger();
        QCOMPARE(thumbnails->thumbnailSize(), 128);
        QVERIFY(!medium->isChecked());
        larger->trigger();
        QCOMPARE(thumbnails->thumbnailSize(), 160);
        QVERIFY(medium->isChecked());

        // Una pestaña nueva sale con el mismo tamaño.
        BrowserPane *other = window.openTab(directory.path());
        QCOMPARE(other->thumbnailView()->thumbnailSize(), 160);
    }

    void videoAndAudioPlayWithSoundInThePreview()
    {
        if (!MediaPreview::isAvailable()) {
            QSKIP("Compilado sin Qt Multimedia.");
        }
        QTemporaryDir data;
        QTemporaryDir directory;
        QVERIFY(eu::testsupport::writeWav(directory.filePath(QStringLiteral("canción.wav")), 3.0));
#ifdef EU_HAVE_FFMPEG
        QVERIFY(eu::testsupport::writeColorVideo(directory.filePath(QStringLiteral("paseo.mov")),
                                                 {{Qt::red, 10}, {Qt::blue, 10}}, 10, QSize(160, 120)));
#endif
        writeFile(directory.filePath(QStringLiteral("notas.txt")), 10);
        writeFile(directory.filePath(QStringLiteral("roto.mp4")), 4000);   // no es un video

        MainWindow window(m_scheduler, openJournal(data));
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        PreviewPanel *panel = window.previewPanel();
        MediaPreview *media = panel->mediaPreview();
        QVERIFY(media != nullptr);

        // Un audio: el reproductor, sin sonar hasta que se pulsa ▶.
        selectNames(pane, {QStringLiteral("canción.wav")});
        QTRY_COMPARE_WITH_TIMEOUT(panel->shown(), PreviewPanel::Shown::Media, 10000);
        QVERIFY(media->isAudio());
        QCOMPARE(QDir::cleanPath(media->path()), QDir::cleanPath(directory.filePath(QStringLiteral("canción.wav"))));
        QVERIFY(!media->isPlaying());
        media->play();
        QTRY_VERIFY_WITH_TIMEOUT(media->durationMs() > 0 || !media->error().isEmpty(), 10000);
        QVERIFY2(media->error().isEmpty(), qPrintable(media->error()));
        QVERIFY2(std::abs(media->durationMs() - 3000) < 300, qPrintable(QString::number(media->durationMs())));
        QVERIFY(media->isPlaying());
        // Se puede saltar a otro punto.
        media->seek(2000);
        QTRY_VERIFY_WITH_TIMEOUT(media->positionMs() >= 1900, 10000);
        // Pausa y sigue (barra espaciadora con el foco en el reproductor).
        media->setFocus();
        QTest::keyClick(media, Qt::Key_Space);
        QVERIFY(!media->isPlaying());
        QTest::keyClick(media, Qt::Key_Space);
        QVERIFY(media->isPlaying());
        // M silencia; el volumen vale para todos los archivos y se guarda.
        QTest::keyClick(media, Qt::Key_M);
        QVERIFY(media->isMuted());
        QTest::keyClick(media, Qt::Key_M);
        QVERIFY(!media->isMuted());
        media->setVolume(35);

        // Volver a leer la carpeta (lo mismo seleccionado) no corta lo que suena.
        selectNames(pane, {QStringLiteral("canción.wav")});
        QVERIFY(media->isPlaying());

        // Otro archivo: para y suelta el anterior.
        selectNames(pane, {QStringLiteral("notas.txt")});
        QTRY_COMPARE_WITH_TIMEOUT(panel->shown(), PreviewPanel::Shown::Text, 10000);
        QVERIFY(!media->isPlaying());
        QCOMPARE(media->positionMs(), 0);

        // Un archivo dañado: lo dice y no se queda reproduciendo.
        selectNames(pane, {QStringLiteral("roto.mp4")});
        QTRY_COMPARE_WITH_TIMEOUT(panel->shown(), PreviewPanel::Shown::Media, 10000);
        media->play();
        QTRY_VERIFY_WITH_TIMEOUT(!media->error().isEmpty(), 10000);
        QVERIFY(!media->isPlaying());

#ifdef EU_HAVE_FFMPEG
        // Un video: su miniatura de portada y, al pulsar ▶, los fotogramas.
        selectNames(pane, {QStringLiteral("paseo.mov")});
        QTRY_COMPARE_WITH_TIMEOUT(panel->shown(), PreviewPanel::Shown::Media, 10000);
        QVERIFY(!media->isAudio());
        QVERIFY(!panel->picture().isNull());
        QVERIFY(!media->hasFrame());
        media->play();
        QTRY_VERIFY_WITH_TIMEOUT(media->hasFrame() || !media->error().isEmpty(), 10000);
        QVERIFY2(media->error().isEmpty(), qPrintable(media->error()));

        // Nunca suenan dos a la vez: la miniatura calla al panel y al revés.
        InlinePlayer *inline_ = window.inlinePlayer();
        inline_->play(directory.filePath(QStringLiteral("paseo.mov")), false);
        QVERIFY(!media->isPlaying());
        media->play();
        QVERIFY(!inline_->isPlaying());

        // Al terminar (dura 2 s), vuelve al principio y suelta el archivo.
        QTRY_VERIFY_WITH_TIMEOUT(!media->isPlaying(), 15000);
        QCOMPARE(media->positionMs(), 0);
        QVERIFY(!media->hasFrame());
#endif

        // Ocultar el panel (Alt+P) para lo que suene.
        selectNames(pane, {QStringLiteral("canción.wav")});
        QTRY_COMPARE_WITH_TIMEOUT(panel->shown(), PreviewPanel::Shown::Media, 10000);
        media->play();
        QVERIFY2(media->isPlaying(), qPrintable(media->error() + QStringLiteral(" ruta=") + media->path()));
        window.setPreviewVisible(false);
        QVERIFY(!media->isPlaying());
        window.setPreviewVisible(true);

        // El volumen se recuerda al cerrar.
        window.close();
        QCOMPARE(QSettings().value(QStringLiteral("view/previewVolume")).toInt(), 35);
        QSettings().remove(QStringLiteral("view/previewVolume"));   // para las demás pruebas
    }

    void archivesAreBrowsedLikeFoldersAndExtracted()
    {
        if (!eu::archive::isAvailable()) {
            QSKIP("Compilado sin libarchive.");
        }
        QTemporaryDir data;
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        QVERIFY(QDir(root).mkpath(QStringLiteral("álbum/sub")));
        writeFile(root + QStringLiteral("/álbum/foto1.txt"), 10);
        writeFile(root + QStringLiteral("/álbum/sub/foto2.txt"), 20);
        writeFile(root + QStringLiteral("/notas.txt"), 5);

        MainWindow window(m_scheduler, openJournal(data));
        BrowserPane *pane = window.openTab(root);
        QVERIFY(waitReady(pane));
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));

        // Comprimir lo seleccionado (lo mismo que hace el menú contextual).
        const QString zip = root + QStringLiteral("/paquete.zip");
        window.fileActions()->compress({root + QStringLiteral("/álbum"), root + QStringLiteral("/notas.txt")}, zip,
                                       eu::archive::Format::Zip);
        QVERIFY(waitIdle(window));
        QVERIFY(QFileInfo(zip).isFile());
        QTRY_VERIFY(pane->model()->rowOfName(QStringLiteral("paquete.zip")) >= 0);

        // Doble clic: se entra en él como en una carpeta.
        emit pane->view()->activated(pane->model()->index(pane->model()->rowOfName(QStringLiteral("paquete.zip")), 0));
        QVERIFY(pane->showsArchive());
        QVERIFY(waitReady(pane));
        QCOMPARE(pane->model()->state(), DirectoryModel::State::Ready);
        QCOMPARE(pane->model()->rowCount(), 2);
        QVERIFY(pane->model()->rowOfName(QStringLiteral("álbum")) >= 0);
        QVERIFY(pane->model()->rowOfName(QStringLiteral("notas.txt")) >= 0);
        // Dentro no se renombra ni se arrastra, ni se busca.
        const Qt::ItemFlags flags = pane->model()->flags(pane->model()->index(0, 0));
        QVERIFY(!(flags & Qt::ItemIsEditable));
        QVERIFY(!(flags & Qt::ItemIsDragEnabled));
        QVERIFY(!window.searchEdit()->isEnabled());

        // Una carpeta de dentro, y vuelta arriba hasta salir del comprimido.
        emit pane->view()->activated(pane->model()->index(pane->model()->rowOfName(QStringLiteral("álbum")), 0));
        QVERIFY(waitReady(pane));
        QCOMPARE(pane->model()->rowCount(), 2);   // foto1.txt y sub
        QVERIFY(pane->model()->rowOfName(QStringLiteral("sub")) >= 0);
        pane->goUp();
        QVERIFY(waitReady(pane));
        QVERIFY(pane->showsArchive());
        QCOMPARE(pane->model()->rowCount(), 2);
        pane->goUp();
        QVERIFY(waitReady(pane));
        QVERIFY(!pane->showsArchive());
        QCOMPARE(pane->currentPath(), root);
        QTRY_COMPARE(pane->model()->entryAt(pane->view()->currentIndex().row()).name, QStringLiteral("paquete.zip"));
        QVERIFY(window.searchEdit()->isEnabled());

        // Extraer todo: en una carpeta nueva con el nombre del comprimido.
        window.fileActions()->extract(zip, {}, QString(), root, QStringLiteral("paquete"));
        QVERIFY(waitIdle(window));
        QVERIFY(QFileInfo(root + QStringLiteral("/paquete/álbum/sub/foto2.txt")).isFile());
        QCOMPARE(QFileInfo(root + QStringLiteral("/paquete/notas.txt")).size(), 5);
        // Lo seleccionado dentro de "álbum": queda arriba, sin "álbum/".
        window.fileActions()->extract(zip, {QStringLiteral("álbum/sub")}, QStringLiteral("álbum"), root,
                                      QStringLiteral("parcial"));
        QVERIFY(waitIdle(window));
        QVERIFY(QFileInfo(root + QStringLiteral("/parcial/sub/foto2.txt")).isFile());
        QVERIFY(!QFileInfo::exists(root + QStringLiteral("/parcial/álbum")));
        QVERIFY(!QFileInfo::exists(root + QStringLiteral("/parcial/foto1.txt")));

        // Extraer aquí, en la misma carpeta del comprimido: lo nuevo queda
        // junto a él y lo que ya existía no se toca.
        const QString here = root + QStringLiteral("/aquí");
        QVERIFY(QDir().mkdir(here));
        QVERIFY(QFile::copy(zip, here + QStringLiteral("/paquete.zip")));
        writeFile(here + QStringLiteral("/notas.txt"), 2);
        QSignalSpy hereReports(&window, &MainWindow::actionReported);
        window.extractArchive(here + QStringLiteral("/paquete.zip"), {}, QString(), false);
        QVERIFY(waitIdle(window));
        QVERIFY(QFileInfo(here + QStringLiteral("/álbum/sub/foto2.txt")).isFile());
        QCOMPARE(QFileInfo(here + QStringLiteral("/notas.txt")).size(), 2);
        QCOMPARE(QDir(here).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 3);   // zip, notas y álbum
        QTRY_COMPARE(hereReports.count(), 1);
        QVERIFY2(hereReports.first().at(0).toString().contains(QStringLiteral("1 elemento(s) nuevo")),
                 qPrintable(hereReports.first().at(0).toString()));
        QVERIFY2(hereReports.first().at(0).toString().contains(QStringLiteral("ya existían")),
                 qPrintable(hereReports.first().at(0).toString()));

        // Comprimir encima de uno que ya existe: error, y el existente sigue igual.
        const qint64 before = QFileInfo(zip).size();
        QSignalSpy reports(&window, &MainWindow::actionReported);
        window.fileActions()->compress({root + QStringLiteral("/notas.txt")}, zip, eu::archive::Format::Zip);
        QVERIFY(waitIdle(window));
        QTRY_COMPARE(reports.count(), 1);
        QCOMPARE(reports.first().at(1).toBool(), true);   // es un error
        QCOMPARE(QFileInfo(zip).size(), before);
    }

    void searchCanBePausedAndStopped()
    {
        QTemporaryDir directory;
        const QString root = QDir::cleanPath(directory.path());
        for (int i = 0; i < 400; ++i) {
            QVERIFY(QDir(root).mkpath(QStringLiteral("c%1").arg(i)));
            writeFile(root + QStringLiteral("/c%1/dato.txt").arg(i));
        }
        MainWindow window(m_scheduler);
        BrowserPane *pane = window.openTab(root);
        QVERIFY(waitReady(pane));

        pane->search(QStringLiteral("dato"));
        QCOMPARE(pane->searchState(), BrowserPane::SearchState::Running);
        pane->setSearchPaused(true);
        QCOMPARE(pane->searchState(), BrowserPane::SearchState::Paused);
        // En pausa deja de avanzar (tras lo que ya venía de camino).
        QTest::qWait(200);
        const qint64 scanned = pane->searchProgress().foldersScanned;
        QTest::qWait(200);
        QCOMPARE(pane->searchProgress().foldersScanned, scanned);
        QVERIFY(scanned < 401);

        pane->setSearchPaused(false);
        QTRY_COMPARE_WITH_TIMEOUT(pane->searchState(), BrowserPane::SearchState::Finished, 20000);
        QCOMPARE(pane->resultsModel()->count(), 400);

        // Detener deja lo encontrado y lo marca como incompleto; salir de los
        // resultados con una búsqueda en marcha no deja nada colgado.
        pane->refresh();
        pane->stopSearch();
        QCOMPARE(pane->searchState(), BrowserPane::SearchState::Stopped);
        QVERIFY(pane->statusText().contains(QStringLiteral("detenida")));
        pane->refresh();
        pane->goBack();
        QVERIFY(pane->showsFolder());
        QCOMPARE(pane->searchState(), BrowserPane::SearchState::Idle);
    }

    void pasteIntoAnotherFolder()
    {
        // Lo que hace "Pegar en esta carpeta" del menú del árbol de lugares:
        // pegar en una carpeta que no es la que se está viendo.
        QTemporaryDir data;
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkdir(QStringLiteral("destino")));
        writeFile(directory.filePath(QStringLiteral("a.txt")), 5);
        writeFile(directory.filePath(QStringLiteral("b.txt")), 5);
        const QString destination = directory.filePath(QStringLiteral("destino"));

        MainWindow window(m_scheduler, openJournal(data));
        window.show();
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));

        selectNames(pane, {QStringLiteral("a.txt")});
        window.copySelection();
        window.pasteInto(destination);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(destination + QStringLiteral("/a.txt")), 10000);
        QVERIFY(waitIdle(window));
        QVERIFY(QFileInfo::exists(directory.filePath(QStringLiteral("a.txt"))));
        QCOMPARE(pane->currentPath(), QDir::cleanPath(directory.path()));   // no se cambia de carpeta

        selectNames(pane, {QStringLiteral("b.txt")});
        window.cutSelection();
        window.pasteInto(destination);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(destination + QStringLiteral("/b.txt")), 10000);
        QVERIFY(waitIdle(window));
        QVERIFY(!QFileInfo::exists(directory.filePath(QStringLiteral("b.txt"))));
    }

    void withoutJournalTheWindowOnlyBrowses()
    {
        QTemporaryDir directory;
        writeFile(directory.filePath(QStringLiteral("a.txt")));
        MainWindow window(m_scheduler);   // sin historial
        BrowserPane *pane = window.openTab(directory.path());
        QVERIFY(waitReady(pane));
        selectNames(pane, {QStringLiteral("a.txt")});
        window.trashSelection();
        window.newFolder();
        QTest::qWait(200);
        QCOMPARE(QDir(directory.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(), 1);
    }

    void sessionIsRestored()
    {
        QTemporaryDir first;
        QTemporaryDir second;
        {
            MainWindow window(m_scheduler);
            window.openTab(first.path());
            window.openTab(second.path());
            window.show();
            QVERIFY(window.close());   // al cerrar se guarda la sesión
        }
        MainWindow restored(m_scheduler);
        restored.restoreSession();
        QCOMPARE(restored.tabCount(), 2);
        QCOMPARE(restored.currentPane()->currentPath(), QDir::cleanPath(second.path()));
    }
};

QTEST_MAIN(TestMainWindow)
#include "test_main_window.moc"
