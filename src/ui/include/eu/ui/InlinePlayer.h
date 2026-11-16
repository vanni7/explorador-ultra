// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>
#include <QObject>
#include <QString>

#include <memory>

namespace eu::ui {

/// Reproduce un video o un audio dentro de su miniatura (SPEC §5.5).
///
/// Hay uno por ventana: así nunca suena ni se decodifica más de una
/// reproducción a la vez. El video va siempre en silencio; el audio suena.
/// Sin Qt Multimedia en la compilación, isAvailable() es false y play() no
/// hace nada.
class InlinePlayer : public QObject
{
    Q_OBJECT

public:
    explicit InlinePlayer(QObject *parent = nullptr);
    ~InlinePlayer() override;

    static bool isAvailable();

    /// Empieza a reproducir `path` (y para lo que sonara antes). Devuelve
    /// false si no se puede.
    bool play(const QString &path, bool isAudio);
    void stop();

    bool isPlaying() const;
    QString path() const { return m_path; }
    bool isAudio() const { return m_isAudio; }
    /// Último fotograma, ya reducido a `maxSide` (vacío en el audio).
    QImage frame() const { return m_frame; }
    void setMaxFrameSide(int side) { m_maxSide = side; }
    qint64 positionMs() const;
    qint64 durationMs() const;
    /// Mensaje del último fallo (archivo que no se puede reproducir).
    QString error() const { return m_error; }

signals:
    /// Fotograma nuevo o avance: hay que repintar.
    void changed();
    /// Se paró (a mano, por terminar o por un error).
    void stopped();

private:
    struct Backend;
    std::unique_ptr<Backend> m_backend;
    QString m_path;
    bool m_isAudio = false;
    QImage m_frame;
    int m_maxSide = 512;
    QString m_error;
};

} // namespace eu::ui
