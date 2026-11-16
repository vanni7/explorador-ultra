// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/BrowserPane.h"
#include "eu/ui/FileClipboard.h"
#include "eu/ui/Locations.h"
#include "eu/ui/MainWindow.h"
#include "eu/ui/PathBar.h"
#include "eu/ui/SearchResultsModel.h"
#include "eu/ui/IndexDialog.h"
#include "eu/archive/Archive.h"
#include "eu/ui/InlinePlayer.h"
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
#include <QLineEdit>
#include <QMimeData>
#include <QProcess>
#include <QItemSelectionModel>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QTreeView>
#include <QUrl>

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

        // Pegar otra vez en la misma carpeta no sobrescribe: crea copias con otro nombre.
        window.paste();
        QTRY_COMPARE_WITH_TIMEOUT(QDir(destination).entryList(QDir::Files).size(), 4, 10000);
        QVERIFY(waitIdle(window));

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

        // Mayús + soltar sobre la carpeta: mueve. Ya hay "a.txt" dentro, así
        // que se omite sin sobrescribir y el original sigue en su sitio.
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
        QVERIFY(dropAt(external.get(), emptyArea, Qt::ShiftModifier));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(directory.filePath(QStringLiteral("externo.txt"))), 10000);
        QVERIFY(waitIdle(window));
        QVERIFY(!QFileInfo::exists(outside.filePath(QStringLiteral("externo.txt"))));

        // Una carpeta no se puede soltar dentro de sí misma, ni texto que no son archivos.
        std::unique_ptr<QMimeData> itself(makeFileMimeData({folder}, false));
        QVERIFY(!dropAt(itself.get(), centerOf(QStringLiteral("carpeta")), Qt::ControlModifier));
        QMimeData text;
        text.setText(QStringLiteral("hola"));
        QVERIFY(!dropAt(&text, emptyArea, Qt::ControlModifier));
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

    void archivesAreBrowsedLikeFoldersAndExtracted()
    {
        if (!archive::isAvailable()) {
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
                                       archive::Format::Zip);
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

        // Comprimir encima de uno que ya existe: error, y el existente sigue igual.
        const qint64 before = QFileInfo(zip).size();
        QSignalSpy reports(&window, &MainWindow::actionReported);
        window.fileActions()->compress({root + QStringLiteral("/notas.txt")}, zip, archive::Format::Zip);
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
