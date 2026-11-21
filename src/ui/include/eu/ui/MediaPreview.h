// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QImage>
#include <QWidget>

#include <memory>

class QLabel;
class QSlider;
class QToolButton;

namespace eu::ui {

class MediaScreen;

/// Reproductor del panel de previsualización (SPEC §7): video y audio con
/// sonido, con botón de reproducir y pausar, barra de avance, tiempo y volumen.
///
/// No abre el archivo hasta que se pulsa ▶ (en Windows, un archivo abierto no
/// se puede renombrar ni borrar) y lo suelta al terminar, al cambiar de
/// archivo y al ocultarse. Con el foco en el reproductor: barra espaciadora
/// (reproducir o pausar), flechas izquierda y derecha (5 s atrás o adelante)
/// y M (silenciar). Sin Qt Multimedia en la compilación, isAvailable() es
/// false y no se crea.
class MediaPreview : public QWidget
{
    Q_OBJECT

public:
    explicit MediaPreview(QWidget *parent = nullptr);
    ~MediaPreview() override;

    static bool isAvailable();

    /// Lo que se reproducirá al pulsar ▶, sin abrirlo aún. `poster` se ve
    /// mientras no hay fotograma: la miniatura del video o la carátula del audio.
    void setMedia(const QString &path, bool isAudio, const QImage &poster);
    /// Para y suelta el archivo; queda listo para volver a empezar.
    void release();

    void play();
    void pause();
    void togglePlayback();
    bool isPlaying() const;
    QString path() const { return m_path; }
    bool isAudio() const { return m_isAudio; }
    qint64 positionMs() const;
    qint64 durationMs() const;
    void seek(qint64 positionMs);
    /// Mensaje del último fallo (el archivo no se pudo reproducir).
    QString error() const { return m_error; }
    /// true si ya se ve algún fotograma del video.
    bool hasFrame() const;

    /// Volumen de 0 a 100, y silencio: valen para todos los archivos.
    void setVolume(int percent);
    int volume() const { return m_volumePercent; }
    void setMuted(bool muted);
    bool isMuted() const { return m_muted; }

signals:
    /// Empezó a sonar (para callar el reproductor de las miniaturas).
    void playbackStarted();
    /// Cambió el volumen o el silencio (para guardarlo).
    void volumeChanged();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void ensureBackend();
    void applyVolume();
    void updateControls();
    void showError(const QString &text);

    struct Backend;
    std::unique_ptr<Backend> m_backend;
    MediaScreen *m_screen = nullptr;
    QToolButton *m_playButton = nullptr;
    QSlider *m_position = nullptr;
    QLabel *m_time = nullptr;
    QToolButton *m_muteButton = nullptr;
    QSlider *m_volume = nullptr;
    QLabel *m_errorLabel = nullptr;
    QString m_path;
    bool m_isAudio = false;
    bool m_loaded = false;   ///< El archivo está abierto en el reproductor.
    /// Se pidió reproducir (el reproductor tarda un instante en abrir el
    /// archivo y decir que reproduce: el botón no debe esperarlo).
    bool m_playing = false;
    QString m_error;
    int m_volumePercent = 80;
    bool m_muted = false;
};

} // namespace eu::ui
