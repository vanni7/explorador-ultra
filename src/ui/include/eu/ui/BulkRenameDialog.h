// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/rename/Rename.h"
#include "eu/tasks/Scheduler.h"

#include <QDialog>
#include <QTimer>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTreeWidget;

namespace eu::ui {

/// Ventana "Renombrar varios" (SPEC §11): alcance, reglas en cadena y vista
/// previa "Nombre actual → Nombre nuevo" con los problemas de cada uno. La
/// vista previa se calcula en segundo plano (lee el disco y, para las
/// plantillas con datos, los metadatos en el proceso auxiliar).
class BulkRenameDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Scope {
        Selection,   ///< Lo seleccionado.
        Folder,      ///< Todo lo de la carpeta.
        Recursive,   ///< La carpeta y sus subcarpetas.
    };

    BulkRenameDialog(tasks::Scheduler &scheduler, QStringList selection, QString folder, QWidget *parent = nullptr);
    ~BulkRenameDialog() override;

    /// Lo que hay que renombrar: (ruta, nombre nuevo) de las filas sin problemas.
    QList<QPair<QString, QString>> renames() const;

    // Para las pruebas.
    void setRules(const QList<rename::Rule> &rules);
    void setExtensionMode(rename::ExtensionMode mode);
    void setScope(Scope scope, bool includeFolders);
    QTreeWidget *preview() const { return m_preview; }
    QPushButton *renameButton() const { return m_renameButton; }
    bool isPreviewCurrent() const { return m_shownGeneration == m_generation; }

signals:
    void previewUpdated();

private:
    void schedulePreview();
    void startPreview();
    void showPreview(quint64 generation, const QList<rename::Proposal> &proposals);
    void addRule(rename::Rule::Kind kind);
    void loadEditor();
    void ruleEdited();
    void refreshRuleList();
    QString describe(const rename::Rule &rule) const;

    tasks::Scheduler &m_scheduler;
    QStringList m_selection;
    QString m_folder;
    rename::Options m_options;
    QList<rename::Proposal> m_proposals;
    quint64 m_generation = 0;
    quint64 m_shownGeneration = 0;
    tasks::TaskHandle m_task;
    QTimer m_debounce;
    bool m_loading = false;

    QComboBox *m_scope = nullptr;
    QCheckBox *m_includeFolders = nullptr;
    QListWidget *m_rules = nullptr;
    QStackedWidget *m_editors = nullptr;
    QComboBox *m_extension = nullptr;
    QTreeWidget *m_preview = nullptr;
    QLabel *m_summary = nullptr;
    QPushButton *m_renameButton = nullptr;

    // Campos de los editores de reglas.
    QComboBox *m_caseMode = nullptr;
    QLineEdit *m_find = nullptr;
    QLineEdit *m_with = nullptr;
    QCheckBox *m_caseSensitive = nullptr;
    QComboBox *m_removeMode = nullptr;
    QLineEdit *m_removeCharacters = nullptr;
    QSpinBox *m_removeCount = nullptr;
    QSpinBox *m_removeFrom = nullptr;
    QLineEdit *m_insertText = nullptr;
    QComboBox *m_insertPosition = nullptr;
    QSpinBox *m_insertAt = nullptr;
    QLineEdit *m_regexFind = nullptr;
    QLineEdit *m_regexWith = nullptr;
    QCheckBox *m_regexCase = nullptr;
    QSpinBox *m_counterStart = nullptr;
    QSpinBox *m_counterStep = nullptr;
    QSpinBox *m_counterDigits = nullptr;
    QComboBox *m_counterPosition = nullptr;
    QLineEdit *m_counterSeparator = nullptr;
    QLineEdit *m_template = nullptr;
};

/// Los datos de un archivo para las plantillas de renombrado, leídos en el
/// proceso auxiliar (EXIF, etiquetas de audio, dimensiones).
rename::Metadata renameMetadataFor(const QString &path, const tasks::CancelToken &cancel);

} // namespace eu::ui
