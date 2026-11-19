// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QString>

class QPrinter;
class QWidget;

/// Impresión (SPEC §19): vista previa con el sistema de impresión nativo
/// (impresora, páginas, orientación y copias).
namespace eu::ui {

enum class PrintKind {
    None,          ///< No se sabe imprimir desde aquí.
    Image,         ///< Fotos e imágenes (decodificadas en el proceso auxiliar).
    Pdf,           ///< Página a página, pintadas en el proceso auxiliar.
    Text,          ///< Texto y código, en letra de ancho fijo; Markdown con su formato.
    Document,      ///< Word, OpenDocument y hojas de cálculo, como en la vista previa.
};

PrintKind printKindFor(const QString &path);

/// Programas, guiones e instaladores: no se imprimen ni se abren desde aquí
/// sin avisar (SPEC §19).
bool isPotentiallyDangerous(const QString &path);

/// Pinta el archivo en `printer`, respetando el intervalo de páginas que tenga
/// (fromPage/toPage). Bloqueante: se llama desde la vista previa o, en las
/// pruebas, con una impresora a PDF. `error` explica por qué no se pudo.
bool printFile(const QString &path, PrintKind kind, QPrinter *printer, QString *error);

/// Ctrl+P: la vista previa de impresión del archivo; si no se sabe imprimir,
/// ofrece abrirlo con su programa (salvo si es un programa o un guion).
void printWithPreview(const QString &path, QWidget *parent);

} // namespace eu::ui
