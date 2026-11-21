// Explorador Ultra — SPDX-License-Identifier: GPL-3.0-or-later
#include "eu/ui/MediaPreview.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <limits>

#ifdef EU_HAVE_QTMULTIMEDIA
#include <QAudioOutput>
#include <QElapsedTimer>
#include <QMediaPlayer>
#include <QtAudio>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>
#endif

namespace eu::ui {

namespace {

/// "3:07" o, si pasa de una hora, "1:03:07".
QString clockText(qint64 milliseconds)
{
    const qint64 seconds = std::max<qint64>(0, milliseconds) / 1000;
    const QString secondsText = QStringLiteral("%1").arg(seconds % 60, 2, 10, QLatin1Char('0'));
    if (seconds < 3600) {
        return QString::number(seconds / 60) + QLatin1Char(':') + secondsText;
    }
    return QString::number(seconds / 3600) + QLatin1Char(':')
        + QStringLiteral("%1").arg((seconds / 60) % 60, 2, 10, QLatin1Char('0')) + QLatin1Char(':') + secondsText;
}

QIcon mediaIcon(const QString &name, QStyle::StandardPixmap fallback)
{
    return QIcon::fromTheme(name, QApplication::style()->standardIcon(fallback));
}

/// Cuánto se salta con las flechas.
constexpr qint64 seekStepMs = 5000;

} // namespace

/// Barra en la que un clic lleva allí donde se hizo (no un trozo más allá,
/// como hace QSlider) y después se puede seguir arrastrando.
class JumpSlider : public QSlider
{
public:
    using QSlider::QSlider;

    /// Se llama con el valor elegido por un clic fuera del tirador.
    std::function<void(int)> onJump;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && maximum() > minimum()) {
            QStyleOptionSlider option;
            initStyleOption(&option);
            const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
            if (!handle.contains(event->position().toPoint())) {
                const QRect groove =
                    style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
                const int x = int(event->position().x()) - groove.x() - handle.width() / 2;
                const int value = QStyle::sliderValueFromPosition(minimum(), maximum(), x,
                                                                  groove.width() - handle.width(),
                                                                  option.upsideDown);
                setSliderPosition(value);
                if (onJump) {
                    onJump(value);
                }
            }
        }
        QSlider::mousePressEvent(event);   // el tirador ya está bajo el puntero: se puede arrastrar
    }
};

/// Donde se ve el video: el fotograma en curso o, mientras no lo hay, la
/// miniatura o la carátula, con un ▶ grande encima si no se está reproduciendo.
class MediaScreen : public QWidget
{
public:
    explicit MediaScreen(QWidget *parent) : QWidget(parent)
    {
        setMinimumSize(1, 1);   // que el panel se pueda estrechar
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
        setFocusPolicy(Qt::StrongFocus);   // un clic la enfoca y el teclado manda
        setCursor(Qt::PointingHandCursor);
    }

    QImage frame;
    QImage poster;
    bool isAudio = false;
    bool playing = false;
    std::function<void()> onClick;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QRect area = rect();
        const bool hasFrame = !frame.isNull();
        const QImage &image = hasFrame ? frame : poster;
        if (!isAudio) {
            painter.fillRect(area, Qt::black);
        }
        if (!image.isNull()) {
            QSize shown = image.deviceIndependentSize().toSize();
            // El video ocupa todo lo que hay; una miniatura o una carátula no se agrandan.
            if (hasFrame || shown.width() > area.width() || shown.height() > area.height()) {
                shown.scale(area.size(), Qt::KeepAspectRatio);
            }
            const QRect target(area.left() + (area.width() - shown.width()) / 2,
                               area.top() + (area.height() - shown.height()) / 2, shown.width(), shown.height());
            painter.drawImage(target, image);
        } else if (isAudio && playing) {
            // Un audio sin carátula: su icono mientras suena.
            const int side = std::min({96, area.width(), area.height()});
            const QIcon icon = mediaIcon(QStringLiteral("audio-x-generic"), QStyle::SP_MediaVolume);
            icon.paint(&painter, QRect(area.center().x() - side / 2, area.center().y() - side / 2, side, side));
        }
        if (!playing) {
            const int side = std::clamp(std::min(area.width(), area.height()) / 4, 36, 72);
            const QRect button(area.center().x() - side / 2, area.center().y() - side / 2, side, side);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(0, 0, 0, 150));
            painter.drawEllipse(button);
            painter.setBrush(Qt::white);
            const QRectF inner = QRectF(button).adjusted(side * 0.36, side * 0.3, -side * 0.26, -side * 0.3);
            const QPointF triangle[3] = {inner.topLeft(), inner.bottomLeft(), QPointF(inner.right(), inner.center().y())};
            painter.drawPolygon(triangle, 3);
        }
        if (hasFocus()) {
            painter.setPen(QPen(palette().color(QPalette::Highlight), 1, Qt::DotLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(area.adjusted(0, 0, -1, -1));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && onClick) {
            setFocus(Qt::MouseFocusReason);
            onClick();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }
};

#ifdef EU_HAVE_QTMULTIMEDIA

struct MediaPreview::Backend
{
    // El reproductor, el último: se destruye antes que la salida y el sumidero que usa.
    QAudioOutput audio;
    QVideoSink sink;
    QMediaPlayer player;
    QElapsedTimer sinceFrame;
};

bool MediaPreview::isAvailable()
{
    return true;
}

#else

struct MediaPreview::Backend
{
};

bool MediaPreview::isAvailable()
{
    return false;
}

#endif

MediaPreview::MediaPreview(QWidget *parent) : QWidget(parent)
{
    setAccessibleName(tr("Reproductor"));
    m_screen = new MediaScreen(this);
    m_screen->setAccessibleName(tr("Imagen del video"));
    m_screen->onClick = [this] { togglePlayback(); };

    auto *position = new JumpSlider(Qt::Horizontal, this);
    position->onJump = [this](int value) { seek(value); };
    m_position = position;
    m_position->setAccessibleName(tr("Avance"));
    m_position->setToolTip(tr("Avance (flechas: 5 segundos atrás o adelante)"));
    m_position->setSingleStep(int(seekStepMs));
    m_position->setPageStep(int(6 * seekStepMs));
    m_position->setEnabled(false);
    connect(m_position, &QSlider::sliderMoved, this, [this](int value) { seek(value); });
    // Las teclas de la barra (flechas, Re Pág, Inicio…) también llevan allí.
    connect(m_position, &QSlider::actionTriggered, this, [this](int action) {
        if (action != QAbstractSlider::SliderMove && action != QAbstractSlider::SliderNoAction) {
            seek(m_position->sliderPosition());
        }
    });

    m_playButton = new QToolButton(this);
    m_playButton->setAutoRaise(true);
    connect(m_playButton, &QToolButton::clicked, this, &MediaPreview::togglePlayback);

    m_time = new QLabel(this);
    m_time->setAccessibleName(tr("Tiempo"));

    m_muteButton = new QToolButton(this);
    m_muteButton->setAutoRaise(true);
    m_muteButton->setCheckable(true);
    m_muteButton->setAccessibleName(tr("Silenciar"));
    m_muteButton->setToolTip(tr("Silenciar (M)"));
    connect(m_muteButton, &QToolButton::toggled, this, &MediaPreview::setMuted);

    m_volume = new JumpSlider(Qt::Horizontal, this);
    m_volume->setRange(0, 100);
    m_volume->setValue(m_volumePercent);
    m_volume->setFixedWidth(80);
    m_volume->setAccessibleName(tr("Volumen"));
    m_volume->setToolTip(tr("Volumen"));
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        setVolume(value);
        if (value > 0 && m_muted) {
            setMuted(false);   // subir el volumen quita el silencio, como en cualquier reproductor
        }
    });

    m_errorLabel = new QLabel(this);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setEnabled(false);
    m_errorLabel->hide();

    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 0, 0, 0);
    buttons->addWidget(m_playButton);
    buttons->addWidget(m_time);
    buttons->addStretch(1);
    buttons->addWidget(m_muteButton);
    buttons->addWidget(m_volume);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(m_screen, 1);
    layout->addWidget(m_position);
    layout->addLayout(buttons);
    layout->addWidget(m_errorLabel);
    updateControls();
}

MediaPreview::~MediaPreview()
{
#ifdef EU_HAVE_QTMULTIMEDIA
    if (m_backend) {
        // Lo que avise el reproductor mientras se destruye ya no debe llegar aquí.
        QObject::disconnect(&m_backend->player, nullptr, this, nullptr);
        QObject::disconnect(&m_backend->sink, nullptr, this, nullptr);
        m_backend->player.stop();
    }
#endif
}

void MediaPreview::ensureBackend()
{
#ifdef EU_HAVE_QTMULTIMEDIA
    if (m_backend) {
        return;
    }
    // Se crea al primer ▶: cargar Qt Multimedia (y FFmpeg, y buscar los
    // dispositivos de sonido) retrasaría abrir la ventana.
    m_backend = std::make_unique<Backend>();
    Backend &backend = *m_backend;
    backend.player.setAudioOutput(&backend.audio);
    backend.player.setVideoOutput(&backend.sink);
    applyVolume();
    connect(&backend.sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame &frame) {
        if (!m_loaded) {
            return;
        }
        // Convertir cada fotograma cuesta; para el panel bastan unos 30 por segundo.
        if (m_backend->sinceFrame.isValid() && m_backend->sinceFrame.elapsed() < 33) {
            return;
        }
        m_backend->sinceFrame.restart();
        QImage image = frame.toImage();
        if (image.isNull()) {
            return;
        }
        // Al tamaño de la pantalla: pintar un 4K reducido en cada repintado sería lento.
        const QSize room = m_screen->size() * m_screen->devicePixelRatioF();
        if (!room.isEmpty() && (image.width() > room.width() || image.height() > room.height())) {
            image = image.scaled(room, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        m_screen->frame = image;
        m_screen->update();
    });
    connect(&backend.player, &QMediaPlayer::positionChanged, this, &MediaPreview::updateControls);
    connect(&backend.player, &QMediaPlayer::durationChanged, this, &MediaPreview::updateControls);
    connect(&backend.player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        // "Parado" solo llega al soltar el archivo, al terminar o por un fallo,
        // y eso ya se atiende aparte.
        if (m_loaded && state != QMediaPlayer::StoppedState) {
            m_playing = state == QMediaPlayer::PlayingState;
        }
        updateControls();
    });
    // Lo que avise el reproductor de un archivo ya soltado no cuenta.
    connect(&backend.player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (!m_loaded) {
            return;
        }
        if (status == QMediaPlayer::EndOfMedia) {
            release();   // al terminar, de vuelta al principio y el archivo libre
        } else if (status == QMediaPlayer::InvalidMedia) {
            showError(m_backend->player.errorString());
        }
    });
    connect(&backend.player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &text) {
        if (m_loaded) {
            showError(text);
        }
    });
#endif
}

void MediaPreview::setMedia(const QString &path, bool isAudio, const QImage &poster)
{
    release();
    m_path = path;
    m_isAudio = isAudio;
    m_error.clear();
    m_errorLabel->hide();
    m_screen->poster = poster;
    m_screen->isAudio = isAudio;
    m_screen->setAccessibleName(isAudio ? tr("Carátula") : tr("Imagen del video"));
    m_screen->update();
    updateControls();
}

void MediaPreview::release()
{
#ifdef EU_HAVE_QTMULTIMEDIA
    if (m_backend && m_loaded) {
        m_loaded = false;   // antes: lo que avise el reproductor al parar ya no cuenta
        m_backend->player.stop();
        m_backend->player.setSource(QUrl());   // suelta el archivo
    }
#endif
    m_loaded = false;
    m_playing = false;
    m_screen->frame = QImage();
    m_screen->update();
    updateControls();
}

void MediaPreview::play()
{
#ifdef EU_HAVE_QTMULTIMEDIA
    if (m_path.isEmpty()) {
        return;
    }
    ensureBackend();
    m_error.clear();
    m_errorLabel->hide();
    if (!m_loaded) {
        m_loaded = true;
        m_backend->sinceFrame.invalidate();
        m_backend->player.setSource(QUrl::fromLocalFile(m_path));
    }
    m_backend->player.play();
    m_playing = true;
    emit playbackStarted();
    updateControls();
#endif
}

void MediaPreview::pause()
{
#ifdef EU_HAVE_QTMULTIMEDIA
    if (m_backend && m_loaded) {
        m_backend->player.pause();
    }
#endif
    m_playing = false;
    updateControls();
}

void MediaPreview::togglePlayback()
{
    if (isPlaying()) {
        pause();
    } else {
        play();
    }
}

bool MediaPreview::isPlaying() const
{
    return m_loaded && m_playing;
}

qint64 MediaPreview::positionMs() const
{
#ifdef EU_HAVE_QTMULTIMEDIA
    return m_backend && m_loaded ? m_backend->player.position() : 0;
#else
    return 0;
#endif
}

qint64 MediaPreview::durationMs() const
{
#ifdef EU_HAVE_QTMULTIMEDIA
    return m_backend && m_loaded ? m_backend->player.duration() : 0;
#else
    return 0;
#endif
}

void MediaPreview::seek(qint64 positionMs)
{
#ifdef EU_HAVE_QTMULTIMEDIA
    if (m_backend && m_loaded && m_backend->player.isSeekable()) {
        const qint64 duration = m_backend->player.duration();
        m_backend->player.setPosition(std::clamp<qint64>(positionMs, 0, duration > 0 ? duration : positionMs));
    }
#else
    Q_UNUSED(positionMs);
#endif
}

bool MediaPreview::hasFrame() const
{
    return !m_screen->frame.isNull();
}

void MediaPreview::setVolume(int percent)
{
    percent = std::clamp(percent, 0, 100);
    if (percent == m_volumePercent) {
        return;
    }
    m_volumePercent = percent;
    {
        const QSignalBlocker blocker(m_volume);
        m_volume->setValue(percent);
    }
    applyVolume();
    emit volumeChanged();
}

void MediaPreview::setMuted(bool muted)
{
    if (muted == m_muted) {
        return;
    }
    m_muted = muted;
    applyVolume();
    updateControls();
    emit volumeChanged();
}

void MediaPreview::applyVolume()
{
#ifdef EU_HAVE_QTMULTIMEDIA
    if (m_backend) {
        // La barra va como se oye (escala logarítmica), no como la amplitud.
        m_backend->audio.setVolume(QtAudio::convertVolume(float(m_volumePercent) / 100.0f,
                                                          QtAudio::LogarithmicVolumeScale,
                                                          QtAudio::LinearVolumeScale));
        m_backend->audio.setMuted(m_muted);
    }
#endif
}

void MediaPreview::showError(const QString &text)
{
    m_error = text.isEmpty() ? tr("formato no compatible") : text;
    release();
    // El detalle de Qt viene en inglés: va al registro.
    qInfo("No se pudo reproducir %s: %s", qPrintable(m_path), qPrintable(m_error));
    m_errorLabel->setText(tr("No se puede reproducir: el archivo está dañado o su formato no es compatible."));
    m_errorLabel->show();
}

void MediaPreview::updateControls()
{
    const bool playing = isPlaying();
    m_screen->playing = playing;
    m_screen->update();
    m_playButton->setIcon(playing ? mediaIcon(QStringLiteral("media-playback-pause"), QStyle::SP_MediaPause)
                                  : mediaIcon(QStringLiteral("media-playback-start"), QStyle::SP_MediaPlay));
    m_playButton->setAccessibleName(playing ? tr("Pausa") : tr("Reproducir"));
    m_playButton->setToolTip(playing ? tr("Pausa (barra espaciadora)") : tr("Reproducir con sonido (barra espaciadora)"));
    m_playButton->setEnabled(!m_path.isEmpty());

    const qint64 duration = durationMs();
    const qint64 position = positionMs();
    m_position->setEnabled(m_loaded && duration > 0);
    if (!m_position->isSliderDown()) {
        const QSignalBlocker blocker(m_position);
        m_position->setRange(0, int(std::min<qint64>(duration, std::numeric_limits<int>::max())));
        m_position->setValue(int(std::min<qint64>(position, std::numeric_limits<int>::max())));
    }
    m_time->setText(duration > 0 ? clockText(position) + QStringLiteral(" / ") + clockText(duration)
                                 : clockText(position));

    {
        const QSignalBlocker blocker(m_muteButton);
        m_muteButton->setChecked(m_muted);
    }
    m_muteButton->setIcon(m_muted ? mediaIcon(QStringLiteral("audio-volume-muted"), QStyle::SP_MediaVolumeMuted)
                                  : mediaIcon(QStringLiteral("audio-volume-high"), QStyle::SP_MediaVolume));
}

void MediaPreview::keyPressEvent(QKeyEvent *event)
{
    if (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::KeypadModifier) {
        switch (event->key()) {
        case Qt::Key_Space:
            togglePlayback();
            event->accept();
            return;
        case Qt::Key_Left:
            seek(positionMs() - seekStepMs);
            event->accept();
            return;
        case Qt::Key_Right:
            seek(positionMs() + seekStepMs);
            event->accept();
            return;
        case Qt::Key_M:
            setMuted(!m_muted);
            event->accept();
            return;
        default:
            break;
        }
    }
    QWidget::keyPressEvent(event);
}

void MediaPreview::hideEvent(QHideEvent *event)
{
    // Otro archivo, el panel oculto u otra pestaña: nada suena a escondidas. Al
    // minimizar la ventana (lo avisa el sistema), la música sigue.
    if (!event->spontaneous()) {
        release();
    }
    QWidget::hideEvent(event);
}

} // namespace eu::ui
