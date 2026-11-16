// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "eu/thumbs/Thumbnail.h"

#include <QCache>
#include <QImage>
#include <QString>

#include <mutex>

namespace eu::thumbs {

/// Caché de miniaturas en memoria (con tope de bytes) y en disco (SPEC §5.5).
///
/// La clave es ruta + tamaño + fecha + lado: si el archivo cambia, su
/// miniatura vieja ya no se encuentra. También recuerda los archivos que no
/// se pudieron leer, para no intentarlo de nuevo cada vez que se ven.
///
/// Se puede usar desde cualquier hilo. Leer y escribir el disco es lento:
/// load() y store() se llaman desde tareas; lookupMemory() es inmediato.
class ThumbnailCache
{
public:
    /// `directory`: carpeta de la caché en disco (se crea si hace falta).
    /// Vacía: solo memoria.
    explicit ThumbnailCache(QString directory, qint64 memoryBudgetBytes = 256LL * 1024 * 1024);

    enum class State {
        Missing,   ///< Hay que generarla.
        Ready,     ///< En `image`.
        Failed,    ///< El archivo no se pudo leer: se muestra el icono del tipo.
    };
    struct Entry
    {
        State state = State::Missing;
        QImage image;
    };

    static QString keyOf(const ThumbnailRequest &request);

    /// Solo memoria; no toca el disco.
    Entry lookupMemory(const ThumbnailRequest &request) const;
    /// Memoria y, si no está, disco (y entonces la sube a memoria).
    Entry load(const ThumbnailRequest &request);
    void store(const ThumbnailRequest &request, const QImage &image);
    void storeFailure(const ThumbnailRequest &request);

    void setMemoryBudget(qint64 bytes);
    qint64 memoryBudget() const;
    qint64 memoryUsed() const;
    /// Vacía la memoria (no el disco).
    void clearMemory();

    /// Borra de disco lo más antiguo hasta que la caché ocupe como mucho
    /// `maxBytes`. Devuelve cuántos archivos borró.
    int pruneDisk(qint64 maxBytes);

    QString directory() const { return m_directory; }

private:
    QString diskPath(const QString &key, const QString &suffix) const;
    void insertMemory(const QString &key, const Entry &entry);

    QString m_directory;
    mutable std::mutex m_mutex;
    /// El coste de cada entrada son sus bytes; QCache tira lo menos usado.
    QCache<QString, Entry> m_memory;
};

} // namespace eu::thumbs
