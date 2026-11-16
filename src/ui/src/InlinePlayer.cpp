// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/InlinePlayer.h"

#ifdef EU_HAVE_QTMULTIMEDIA
#include <QAudioOutput>
#include <QElapsedTimer>
#include <QMediaPlayer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#endif

namespace eu::ui {

#ifdef EU_HAVE_QTMULTIMEDIA

struct InlinePlayer::Backend
{
    QMediaPlayer player;
    QVideoSink sink;
    QAudioOutput audio;
    QElapsedTimer sinceFrame;
};

InlinePlayer::InlinePlayer(QObject *parent) : QObject(parent), m_backend(std::make_unique<Backend>())
{
    Backend &backend = *m_backend;
    backend.player.setVideoSink(&backend.sink);
    connect(&backend.sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        // Convertir cada fotograma de un 1080p cuesta; para una miniatura
        // bastan unos 25 por segundo.
        if (m_backend->sinceFrame.isValid() && m_backend->sinceFrame.elapsed() < 40) {
            return;
        }
        m_backend->sinceFrame.restart();
        QImage image = frame.toImage();
        if (image.isNull()) {
            return;
        }
        if (image.width() > m_maxSide || image.height() > m_maxSide) {
            image = image.scaled(m_maxSide, m_maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        m_frame = image;
        emit changed();
    });
    connect(&backend.player, &QMediaPlayer::positionChanged, this, &InlinePlayer::changed);
    connect(&backend.player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia || status == QMediaPlayer::InvalidMedia) {
            stop();
        }
    });
    connect(&backend.player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &text) {
        m_error = text;
        stop();
    });
}

InlinePlayer::~InlinePlayer() = default;

bool InlinePlayer::isAvailable()
{
    return true;
}

bool InlinePlayer::play(const QString &path, bool isAudio)
{
    stop();
    m_path = path;
    m_isAudio = isAudio;
    m_error.clear();
    m_frame = QImage();
    Backend &backend = *m_backend;
    // El video, siempre en silencio (SPEC §5.5); el audio, con sonido.
    backend.player.setAudioOutput(isAudio ? &backend.audio : nullptr);
    backend.player.setSource(QUrl::fromLocalFile(path));
    backend.sinceFrame.invalidate();
    backend.player.play();
    emit changed();
    return true;
}

void InlinePlayer::stop()
{
    if (m_path.isEmpty()) {
        return;
    }
    m_backend->player.stop();
    m_backend->player.setSource(QUrl());   // suelta el archivo (en Windows, lo bloquearía)
    m_path.clear();
    m_frame = QImage();
    emit stopped();
    emit changed();
}

bool InlinePlayer::isPlaying() const
{
    return !m_path.isEmpty();
}

qint64 InlinePlayer::positionMs() const
{
    return m_backend->player.position();
}

qint64 InlinePlayer::durationMs() const
{
    return m_backend->player.duration();
}

#else

struct InlinePlayer::Backend
{
};

InlinePlayer::InlinePlayer(QObject *parent) : QObject(parent) {}
InlinePlayer::~InlinePlayer() = default;

bool InlinePlayer::isAvailable()
{
    return false;
}

bool InlinePlayer::play(const QString &, bool)
{
    return false;
}

void InlinePlayer::stop() {}

bool InlinePlayer::isPlaying() const
{
    return false;
}

qint64 InlinePlayer::positionMs() const
{
    return 0;
}

qint64 InlinePlayer::durationMs() const
{
    return 0;
}

#endif

} // namespace eu::ui
