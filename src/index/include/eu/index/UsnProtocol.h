// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Protocolo entre la aplicación y el servicio opcional de Windows que lee el
// diario de cambios (USN Journal) y la MFT de NTFS: eu-indexsvc (SPEC §6.4,
// ADR-0013). Solo C++ estándar: el servicio no usa Qt.
//
// Una conexión lleva una petición y su respuesta. La petición es una línea
// de texto UTF-8 terminada en '\n', con los campos separados por tabuladores
// (un nombre de archivo de Windows no puede contener ninguno de los dos). La
// respuesta empieza con "ok" o con "error":
//
//   hola                                     → ok <versión>
//   posicion <unidad>                        → ok <diario> <usn>
//   cambios  <unidad> <diario> <desde> <máx> → ok <diario> <usn> <n>
//                                              y n líneas, una carpeta cada una
//   recuento <unidad>                        → ok <archivos> <carpetas>
//   (cualquiera)                             → error <código> <explicación>
//
// <unidad> es una letra y dos puntos ("C:"); los números van en decimal.
// "cambios" contesta las carpetas cuyo contenido cambió desde <desde>, solo
// las que quien pregunta puede listar, y la posición desde la que seguir.

#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eu::index::usn {

inline constexpr int protocolVersion = 1;

/// Nombre del servicio en el administrador de servicios de Windows.
inline constexpr wchar_t serviceName[] = L"ExploradorUltraIndice";
/// La tubería por la que contesta.
inline constexpr wchar_t pipeName[] = L"\\\\.\\pipe\\explorador-ultra-indice";
/// Variable de entorno con otra tubería (pruebas, con el servicio en modo
/// consola). Con ella la aplicación no comprueba que al otro lado esté el
/// servicio instalado.
inline constexpr char pipeVariable[] = "EU_INDEXSVC_TUBERIA";

// Códigos de error.
inline constexpr char errorRequest[] = "peticion";               ///< Petición mal formada.
inline constexpr char errorVolume[] = "unidad";                  ///< No se pudo abrir la unidad.
inline constexpr char errorNoJournal[] = "sin-diario";           ///< La unidad no tiene diario activo.
inline constexpr char errorJournalChanged[] = "diario-cambiado"; ///< El diario se borró y se volvió a crear.
inline constexpr char errorPositionLost[] = "desde-perdido";     ///< Lo pedido ya no está en el diario.
inline constexpr char errorTooMany[] = "demasiados";             ///< Cambió más de lo pedido: mejor recorrer.
inline constexpr char errorImpersonation[] = "suplantacion";     ///< No se pudo actuar como quien pregunta.
inline constexpr char errorSystem[] = "sistema";                 ///< Otro fallo de Windows.

/// Línea más larga que se acepta en una petición.
inline constexpr std::size_t maxRequestBytes = 4096;
/// Máximo de carpetas que se puede pedir en "cambios".
inline constexpr std::uint64_t maxFoldersLimit = 100000;

/// Los campos de una línea (sin el '\n' final).
inline std::vector<std::string> splitFields(std::string_view line)
{
    if (!line.empty() && line.back() == '\n') {
        line.remove_suffix(1);
    }
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t tab = line.find('\t', start);
        fields.emplace_back(line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start));
        if (tab == std::string_view::npos) {
            break;
        }
        start = tab + 1;
    }
    return fields;
}

/// Un texto sin tabuladores ni saltos de línea (para meterlo en un campo).
inline std::string sanitized(std::string_view text)
{
    std::string clean(text);
    for (char &c : clean) {
        if (c == '\t' || c == '\n' || c == '\r') {
            c = ' ';
        }
    }
    return clean;
}

/// Una línea con estos campos, terminada en '\n'.
inline std::string makeLine(std::initializer_list<std::string_view> fields)
{
    std::string line;
    bool first = true;
    for (const std::string_view field : fields) {
        if (!first) {
            line += '\t';
        }
        first = false;
        line += sanitized(field);
    }
    line += '\n';
    return line;
}

/// true si es una letra de unidad y dos puntos ("C:"): lo único que el
/// servicio acepta como unidad (nada de rutas ni de "\\.\…").
inline bool isDrive(std::string_view text)
{
    return text.size() == 2 && ((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z'))
        && text[1] == ':';
}

/// Un número sin signo en decimal, entero y sin nada más.
inline std::optional<std::uint64_t> toUnsigned(std::string_view text)
{
    std::uint64_t value = 0;
    if (text.empty() || text.size() > 20) {
        return std::nullopt;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

/// Un número con signo en decimal, entero y sin nada más.
inline std::optional<std::int64_t> toSigned(std::string_view text)
{
    std::int64_t value = 0;
    if (text.empty() || text.size() > 20) {
        return std::nullopt;
    }
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc() || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

} // namespace eu::index::usn
