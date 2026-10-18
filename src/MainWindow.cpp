#include "MainWindow.h"

#include "Exporter.h"
#include "TimeFormat.h"
#include "TrimSlider.h"
#include "VideoView.h"

#include <QAudioOutput>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMediaMetaData>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QStyle>
#include <QTemporaryDir>
#include <QTimeEdit>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

namespace {

constexpr qint64 kMaxTimeEditMs = 24 * 3600 * 1000 - 1;

QTime toTime(qint64 ms)
{
    return QTime::fromMSecsSinceStartOfDay(int(qBound<qint64>(0, ms, kMaxTimeEditMs)));
}

} // namespace

MainWindow::MainWindow(QWidget *parent) : QWidget(parent)
{
    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_player->setAudioOutput(m_audio);

    buildUi();
    bindShortcuts();
    m_player->setVideoSink(m_view->sink());

    m_seekTimer.setSingleShot(true);
    m_seekTimer.setInterval(40);
    connect(&m_seekTimer, &QTimer::timeout, this, [this] {
        if (m_pendingSeek != m_sentSeek)
            flushSeek();
    });

    connect(m_player, &QMediaPlayer::durationChanged, this, &MainWindow::onDurationChanged);
    connect(m_player, &QMediaPlayer::positionChanged, this, &MainWindow::onPositionChanged);
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, &MainWindow::onMediaStatusChanged);
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        if (state == QMediaPlayer::PlayingState)
            m_positionHeld = false;
        m_playButton->setIcon(style()->standardIcon(state == QMediaPlayer::PlayingState ? QStyle::SP_MediaPause
                                                                                        : QStyle::SP_MediaPlay));
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &message) {
        if (!m_previewJob)
            m_status->setText(tr("Playback error: %1").arg(message.toHtmlEscaped()));
    });

    connect(m_view, &VideoView::pointerMoved, this, &MainWindow::showPointer);
    connect(m_view, &VideoView::pointerLeft, this, [this] { m_pointerLabel->setText(tr("Pointer   –")); });
    connect(m_view, &VideoView::cropChanged, this, &MainWindow::showCrop);

    connect(m_timeline, &TrimSlider::seekRequested, this, &MainWindow::seek);
    connect(m_timeline, &TrimSlider::cutDrawn, this, &MainWindow::addCut);
    connect(m_timeline, &TrimSlider::cutSelected, this, [this](int id) { selectCut(id, false); });
    connect(m_timeline, &TrimSlider::startDragged, this, [this](int id, qint64 ms) {
        m_cuts.setStart(id, ms);
        refreshCuts();
    });
    connect(m_timeline, &TrimSlider::endDragged, this, [this](int id, qint64 ms) {
        m_cuts.setEnd(id, ms);
        refreshCuts();
    });
    connect(m_timeline, &TrimSlider::cutDragged, this, [this](int id, qint64 ms) {
        m_cuts.move(id, ms);
        refreshCuts();
    });

    setAcceptDrops(true);
    setWindowTitle(tr("VCrop"));
    resize(1320, 800);
    showCrop(QRect());
    refreshCuts();
}

MainWindow::~MainWindow()
{
    stopPreviewCopy();
}

void MainWindow::buildUi()
{
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    m_view = new VideoView;
    m_view->setMinimumSize(480, 270);

    m_pointerLabel = new QLabel(tr("Pointer   –"));
    m_pointerLabel->setFont(fixed);
    m_cropLabel = new QLabel;
    m_cropLabel->setFont(fixed);
    auto *infoRow = new QHBoxLayout;
    infoRow->addWidget(m_pointerLabel);
    infoRow->addStretch();
    infoRow->addWidget(m_cropLabel);

    m_timeline = new TrimSlider;

    m_playButton = new QToolButton;
    m_playButton->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    m_playButton->setToolTip(tr("Play / pause (Space)"));
    m_playButton->setFocusPolicy(Qt::NoFocus);
    connect(m_playButton, &QToolButton::clicked, this, &MainWindow::togglePlay);

    m_muteButton = new QToolButton;
    m_muteButton->setCheckable(true);
    m_muteButton->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
    m_muteButton->setToolTip(tr("Mute (M)"));
    m_muteButton->setFocusPolicy(Qt::NoFocus);
    connect(m_muteButton, &QToolButton::toggled, this, [this](bool muted) {
        m_audio->setMuted(muted);
        m_muteButton->setIcon(style()->standardIcon(muted ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
    });

    m_timeLabel = new QLabel;
    m_timeLabel->setFont(fixed);

    const auto makeTimeEdit = [] {
        auto *edit = new QTimeEdit;
        edit->setDisplayFormat(QStringLiteral("HH:mm:ss.zzz"));
        edit->setKeyboardTracking(false);
        return edit;
    };
    m_startEdit = makeTimeEdit();
    m_endEdit = makeTimeEdit();
    // The Start and End fields fine-tune whichever trim is selected.
    m_startEdit->setToolTip(tr("Start of the selected trim"));
    m_endEdit->setToolTip(tr("End of the selected trim"));
    connect(m_startEdit, &QTimeEdit::timeChanged, this, [this](QTime t) {
        m_cuts.setStart(m_selectedCut, t.msecsSinceStartOfDay());
        refreshCuts();
        if (const Cut *c = m_cuts.find(m_selectedCut))
            seek(c->start);
    });
    connect(m_endEdit, &QTimeEdit::timeChanged, this, [this](QTime t) {
        m_cuts.setEnd(m_selectedCut, t.msecsSinceStartOfDay());
        refreshCuts();
        if (const Cut *c = m_cuts.find(m_selectedCut))
            seek(c->end);
    });

    m_markStartButton = new QPushButton(tr("Trim from here"));
    m_markStartButton->setToolTip(tr("Start cutting a section out at the current position (I)"));
    connect(m_markStartButton, &QPushButton::clicked, this, &MainWindow::markTrimStart);
    m_markEndButton = new QPushButton(tr("Trim to here"));
    m_markEndButton->setToolTip(tr("Cut out everything from the marked start to the current position (O).\n"
                                   "Without a marked start, everything before the current position is cut."));
    connect(m_markEndButton, &QPushButton::clicked, this, &MainWindow::markTrimEnd);
    m_summaryLabel = new QLabel;
    m_summaryLabel->setFont(fixed);

    auto *transportRow = new QHBoxLayout;
    transportRow->addWidget(m_playButton);
    transportRow->addWidget(m_muteButton);
    transportRow->addWidget(m_timeLabel);
    transportRow->addSpacing(12);
    transportRow->addWidget(m_markStartButton);
    transportRow->addWidget(m_markEndButton);
    transportRow->addStretch();
    transportRow->addWidget(new QLabel(tr("Selected trim")));
    transportRow->addWidget(m_startEdit);
    transportRow->addWidget(new QLabel(tr("to")));
    transportRow->addWidget(m_endEdit);
    transportRow->addSpacing(12);
    transportRow->addWidget(m_summaryLabel);

    // The list of trims, beside the video.
    auto *cutTitle = new QLabel(tr("Trims – sections cut out"));
    QFont titleFont = cutTitle->font();
    titleFont.setBold(true);
    cutTitle->setFont(titleFont);
    m_cutListView = new QListWidget;
    m_cutListView->setFont(fixed);
    m_cutListView->setFocusPolicy(Qt::NoFocus);
    connect(m_cutListView, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        // Selecting rebuilds the list, which must not happen while the list
        // is still in the middle of reporting the change.
        const int id = item ? item->data(Qt::UserRole).toInt() : 0;
        QMetaObject::invokeMethod(this, [this, id] { selectCut(id, true); }, Qt::QueuedConnection);
    });
    m_cutHint = new QLabel(tr("Nothing is cut out yet.\n\n"
                              "Drag in the lane under the timeline to mark a section, or press I where a "
                              "section should start and O where it should end.\n\n"
                              "Drag a section or its edges to adjust it. Playback skips what is cut."));
    m_cutHint->setWordWrap(true);
    m_cutHint->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_cutHint->setEnabled(false);
    m_removeCutButton = new QPushButton(tr("Remove"));
    m_removeCutButton->setToolTip(tr("Remove the selected trim (Delete)"));
    connect(m_removeCutButton, &QPushButton::clicked, this, &MainWindow::removeSelectedCut);
    m_clearCutsButton = new QPushButton(tr("Clear all"));
    connect(m_clearCutsButton, &QPushButton::clicked, this, [this] {
        m_cuts.clear();
        m_mark = -1;
        selectCut(0, false);
    });
    auto *cutButtons = new QHBoxLayout;
    cutButtons->addWidget(m_removeCutButton);
    cutButtons->addWidget(m_clearCutsButton);
    auto *cutPanel = new QWidget;
    cutPanel->setFixedWidth(330);
    auto *cutLayout = new QVBoxLayout(cutPanel);
    cutLayout->setContentsMargins(0, 0, 0, 0);
    cutLayout->addWidget(cutTitle);
    cutLayout->addWidget(m_cutListView, 1);
    cutLayout->addWidget(m_cutHint, 1);
    cutLayout->addLayout(cutButtons);

    m_resetCropButton = new QPushButton(tr("Reset crop"));
    connect(m_resetCropButton, &QPushButton::clicked, m_view, &VideoView::clearCrop);

    m_reencodeCheck = new QCheckBox(tr("Re-encode"));
    m_reencodeCheck->setToolTip(tr("Off: the video is copied as it is, which is instant and lossless, but a kept part\n"
                                   "can only start on a keyframe, so it may begin slightly before the chosen point,\n"
                                   "and a trim too short to contain a keyframe cannot be cut at all.\n"
                                   "On: the video is re-encoded with the preset on the right, and cut exactly.\n"
                                   "A crop always re-encodes."));
    connect(m_reencodeCheck, &QCheckBox::clicked, this, [this](bool checked) {
        m_reencodeWanted = checked;
        updateControls();
    });

    m_presetCombo = new QComboBox;
    m_presetCombo->addItem(tr("H.265 perceptually lossless (RF 20)"), int(ExportPreset::HevcQuality));
    m_presetCombo->addItem(tr("Maximum compatibility (H.264)"), int(ExportPreset::Compatible));
    m_presetCombo->setItemData(0, tr("x265, constant quality 20, slow preset, 10-bit kept for 10-bit sources.\n"
                                     "Small files that look like the source; slow to encode."), Qt::ToolTipRole);
    m_presetCombo->setItemData(1, tr("8-bit H.264 (x264, CRF 18, veryfast). Plays on anything; fast to encode."),
                               Qt::ToolTipRole);
    connect(m_presetCombo, &QComboBox::currentIndexChanged, this, &MainWindow::updateControls);

    m_hardwareCheck = new QCheckBox(tr("Hardware encoder"));
    m_hardwareCheck->setToolTip(tr("Maximum compatibility only: encode H.264 on the GPU where FFmpeg supports it\n"
                                   "(VideoToolbox, NVENC or VA-API). Faster, at somewhat lower quality per megabyte."));

    m_exportButton = new QPushButton(tr("Export…"));
    m_exportButton->setToolTip(tr("Save the cropped and trimmed video (%1)")
                                   .arg(QKeySequence(Qt::CTRL | Qt::Key_E).toString(QKeySequence::NativeText)));
    connect(m_exportButton, &QPushButton::clicked, this, &MainWindow::startExport);

    auto *actionRow = new QHBoxLayout;
    actionRow->addWidget(m_resetCropButton);
    actionRow->addStretch();
    actionRow->addWidget(m_reencodeCheck);
    actionRow->addWidget(m_presetCombo);
    actionRow->addWidget(m_hardwareCheck);
    actionRow->addSpacing(12);
    actionRow->addWidget(m_exportButton);

    m_status = new QLabel;
    m_status->setTextFormat(Qt::RichText);
    m_status->setOpenExternalLinks(true);
    m_status->setWordWrap(true);
    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    m_progress->setFixedWidth(260);
    m_progress->hide();
    m_cancelButton = new QPushButton(tr("Cancel"));
    m_cancelButton->hide();
    connect(m_cancelButton, &QPushButton::clicked, this, [this] {
        if (m_exporter)
            m_exporter->cancel();
    });
    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(m_status, 1);
    statusRow->addWidget(m_progress);
    statusRow->addWidget(m_cancelButton);

    auto *topRow = new QHBoxLayout;
    topRow->addWidget(m_view, 1);
    topRow->addWidget(cutPanel);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(topRow, 1);
    layout->addLayout(infoRow);
    layout->addWidget(m_timeline);
    layout->addLayout(transportRow);
    layout->addLayout(actionRow);
    layout->addLayout(statusRow);
}

void MainWindow::bindShortcuts()
{
    const auto bind = [this](const QKeySequence &keys, auto &&action) {
        connect(new QShortcut(keys, this), &QShortcut::activated, this, std::forward<decltype(action)>(action));
    };
    bind(Qt::Key_Space, [this] { togglePlay(); });
    bind(Qt::Key_M, [this] { m_muteButton->toggle(); });
    bind(Qt::Key_I, [this] { m_markStartButton->click(); });
    bind(Qt::Key_O, [this] { m_markEndButton->click(); });
    bind(Qt::Key_Delete, [this] { removeSelectedCut(); });
    bind(Qt::Key_Backspace, [this] { removeSelectedCut(); });
    bind(Qt::Key_Home, [this] { seek(0); });
    bind(Qt::Key_End, [this] { seek(m_duration); });
    bind(Qt::Key_Left, [this] { step(-1000); });
    bind(Qt::Key_Right, [this] { step(1000); });
    bind(Qt::SHIFT | Qt::Key_Left, [this] { step(-5000); });
    bind(Qt::SHIFT | Qt::Key_Right, [this] { step(5000); });
    bind(Qt::Key_Comma, [this] { step(-frameMs()); });
    bind(Qt::Key_Period, [this] { step(frameMs()); });
    bind(QKeySequence::Open, [this] { chooseFile(); });
    bind(Qt::CTRL | Qt::Key_E, [this] { m_exportButton->click(); });
}

void MainWindow::updateControls()
{
    const bool ready = hasMedia() && m_duration > 0;
    const bool exporting = m_exporter;
    const bool selected = m_cuts.find(m_selectedCut);
    for (QWidget *w : {static_cast<QWidget *>(m_playButton), static_cast<QWidget *>(m_timeline),
                       static_cast<QWidget *>(m_markStartButton), static_cast<QWidget *>(m_markEndButton)})
        w->setEnabled(ready);
    m_startEdit->setEnabled(ready && selected);
    m_endEdit->setEnabled(ready && selected);
    m_removeCutButton->setEnabled(selected);
    m_clearCutsButton->setEnabled(!m_cuts.isEmpty());
    m_resetCropButton->setEnabled(m_view->hasCrop());
    const bool cropped = m_view->hasCrop();
    const bool reencode = cropped || m_reencodeWanted;
    m_reencodeCheck->setChecked(reencode);
    m_reencodeCheck->setEnabled(!cropped);
    m_presetCombo->setEnabled(reencode);
    m_hardwareCheck->setEnabled(reencode
                                && m_presetCombo->currentData().toInt() == int(ExportPreset::Compatible));
    m_exportButton->setEnabled(ready && !exporting);
}

void MainWindow::openFile(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile()) {
        QMessageBox::warning(this, tr("VCrop"), tr("%1 is not a file.").arg(path));
        return;
    }
    stopPreviewCopy();
    m_previewTried = false;
    m_path = info.absoluteFilePath();
    m_duration = 0;
    m_lastPosition = 0;
    m_positionHeld = false;
    m_priming = false;
    m_cuts.clear();
    m_cuts.setDuration(0);
    m_selectedCut = 0;
    m_mark = -1;
    m_view->reset();
    m_timeline->setDuration(0);
    m_status->clear();
    setWindowTitle(tr("%1 — VCrop").arg(info.fileName()));
    setWindowFilePath(m_path);
    m_player->setSource(QUrl::fromLocalFile(m_path));
    m_previewDir.reset(); // only now that the player has let go of the old copy
    showCrop(m_view->crop());
    refreshCuts();
}

void MainWindow::chooseFile()
{
    const QString dir = hasMedia() ? QFileInfo(m_path).absolutePath() : QString();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open video"), dir,
        tr("Videos (*.mp4 *.m4v *.mov *.mkv *.webm *.avi *.ts *.mts *.flv *.wmv *.mpg *.mpeg *.ogv *.3gp);;All files (*)"));
    if (!path.isEmpty())
        openFile(path);
}

void MainWindow::onMediaStatusChanged(QMediaPlayer::MediaStatus status)
{
    if (status == QMediaPlayer::LoadedMedia) {
        if (!m_player->hasVideo()) {
            m_status->setText(tr("This file has no video track."));
            return;
        }
        // Pausing (rather than staying stopped) makes the player show the first frame.
        m_player->pause();
        primeSeeking();
    } else if (status == QMediaPlayer::InvalidMedia) {
        if (hasMedia() && !m_previewTried)
            startPreviewCopy();
        else
            m_status->setText(tr("This file could not be opened as a video."));
    }
}

void MainWindow::startPreviewCopy()
{
    m_previewTried = true;
    m_previewDir = std::make_unique<QTemporaryDir>();
    if (!m_previewDir->isValid()) {
        m_status->setText(tr("This file could not be opened as a video."));
        return;
    }
    ExportJob job;
    job.input = m_path;
    job.output = m_previewDir->filePath(QStringLiteral("preview.mov"));
    job.preview = true;

    m_previewJob = new Exporter(job, this);
    connect(m_previewJob, &Exporter::progress, this, [this](double f) { m_progress->setValue(int(f * 1000)); });
    connect(m_previewJob, &Exporter::succeeded, this, [this, copy = job.output] {
        m_previewJob = nullptr;
        m_progress->hide();
        m_status->clear();
        m_player->setSource(QUrl::fromLocalFile(copy));
    });
    connect(m_previewJob, &Exporter::failed, this, [this](const QString &error) {
        m_previewJob = nullptr;
        m_progress->hide();
        m_status->setText(tr("This video can't be previewed here: %1").arg(error.toHtmlEscaped()));
    });
    connect(m_previewJob, &QThread::finished, m_previewJob, &QObject::deleteLater);

    m_status->setText(tr("The player can't read this file directly; making a temporary copy for the preview…"));
    m_progress->setValue(0);
    m_progress->show();
    m_previewJob->start();
}

void MainWindow::stopPreviewCopy()
{
    if (!m_previewJob)
        return;
    disconnect(m_previewJob, nullptr, this, nullptr);
    m_previewJob->cancel();
    m_previewJob->wait();
    m_previewJob = nullptr;
    m_progress->hide();
}

void MainWindow::onDurationChanged(qint64 ms)
{
    m_duration = qMax<qint64>(0, ms);
    m_timeline->setDuration(m_duration);
    const QTime max = toTime(m_duration);
    m_startEdit->setTimeRange(QTime(0, 0), max);
    m_endEdit->setTimeRange(QTime(0, 0), max);
    m_cuts.setDuration(m_duration);
    refreshCuts();
    showPosition(m_lastPosition);
}

// Qt's AVFoundation player seeks in units of its clock, and until it has
// played once that clock counts whole seconds, so every scrub would land on
// a second boundary. Running it for an instant, muted, fixes the clock.
void MainWindow::primeSeeking()
{
#ifdef Q_OS_MACOS
    if (m_priming)
        return;
    m_priming = true;
    m_audio->setMuted(true);
    m_player->play();
    QTimer::singleShot(1500, this, &MainWindow::finishPriming); // in case it never starts
#endif
}

void MainWindow::finishPriming()
{
    if (!m_priming)
        return;
    m_priming = false;
    m_player->pause();
    m_player->setPosition(0);
    m_audio->setMuted(m_muteButton->isChecked());
    m_lastPosition = 0;
    m_positionHeld = true;
    showPosition(0);
}

void MainWindow::showPosition(qint64 ms)
{
    m_timeline->setPosition(ms);
    m_timeLabel->setText(QStringLiteral("%1 / %2").arg(formatTime(ms), formatTime(m_duration)));
}

void MainWindow::setPlayerPosition(qint64 ms)
{
#ifdef Q_OS_MACOS
    // The same player computes the target in single precision and rounds it
    // down, which lands one frame early when asked for an exact frame time.
    ms = qMin(ms + 1, m_duration);
#endif
    m_player->setPosition(ms);
}

void MainWindow::onPositionChanged(qint64 ms)
{
    if (m_priming) {
        if (ms > 0)
            finishPriming();
        return;
    }
    // While a scrub is in flight the player still reports stale positions.
    if (m_seekTimer.isActive())
        return;
    const bool playing = m_player->playbackState() == QMediaPlayer::PlayingState;
    if (!playing && m_positionHeld)
        return;
    showPosition(ms);

    // Playback that runs into a trim jumps over it, so what plays is what
    // the export will contain. Starting inside a trim plays it, which is how
    // its contents can be reviewed.
    const qint64 from = m_lastPosition;
    m_lastPosition = ms;
    if (!playing)
        return;
    if (const Cut *c = m_cuts.entered(from, ms)) {
        if (c->end >= m_duration - 20) {
            // Nothing is kept after this one: the result ends here.
            m_player->pause();
            seek(c->start);
        } else {
            m_lastPosition = c->end;
            setPlayerPosition(c->end);
        }
    }
}

void MainWindow::togglePlay()
{
    if (!hasMedia())
        return;
    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        m_player->pause();
        return;
    }
    // From the end of what is kept, play starts over at its beginning.
    const qint64 pos = currentPosition();
    const QList<Cut> &cuts = m_cuts.cuts();
    const bool tailCut = !cuts.isEmpty() && cuts.constLast().end >= m_duration - 20;
    const qint64 keptEnd = tailCut ? cuts.constLast().start : m_duration;
    if (pos >= keptEnd - 50 || m_player->mediaStatus() == QMediaPlayer::EndOfMedia) {
        const qint64 keptStart = !cuts.isEmpty() && cuts.constFirst().start == 0 ? cuts.constFirst().end : 0;
        m_lastPosition = keptStart;
        setPlayerPosition(keptStart);
    }
    m_player->play();
}

qint64 MainWindow::currentPosition() const
{
    if (m_seekTimer.isActive())
        return m_pendingSeek;
    return m_player->playbackState() == QMediaPlayer::PlayingState ? m_player->position() : m_lastPosition;
}

void MainWindow::seek(qint64 ms)
{
    if (m_duration <= 0)
        return;
    m_pendingSeek = qBound<qint64>(0, ms, m_duration);
    m_lastPosition = m_pendingSeek;
    m_positionHeld = m_player->playbackState() != QMediaPlayer::PlayingState;
    showPosition(m_pendingSeek);
    if (!m_seekTimer.isActive())
        flushSeek();
}

void MainWindow::flushSeek()
{
    m_sentSeek = m_pendingSeek;
    setPlayerPosition(m_pendingSeek);
    m_seekTimer.start();
}

void MainWindow::step(qint64 deltaMs)
{
    seek(currentPosition() + deltaMs);
}

qint64 MainWindow::frameMs() const
{
    const qreal fps = m_player->metaData().value(QMediaMetaData::VideoFrameRate).toReal();
    return fps > 1 ? qMax<qint64>(1, qRound64(1000.0 / fps)) : 33;
}

void MainWindow::refreshCuts()
{
    if (!m_cuts.find(m_selectedCut))
        m_selectedCut = 0;
    const QList<Cut> &cuts = m_cuts.cuts();
    m_timeline->setCuts(cuts, m_selectedCut);
    m_timeline->setMark(m_mark);

    {
        const QSignalBlocker block(m_cutListView);
        m_cutListView->clear();
        for (int i = 0; i < cuts.size(); ++i) {
            const Cut &c = cuts[i];
            QPixmap swatch(14, 14);
            swatch.fill(Qt::transparent);
            QPainter painter(&swatch);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(CutList::color(c.id));
            painter.drawRoundedRect(swatch.rect(), 3, 3);
            painter.end();

            auto *item = new QListWidgetItem(
                QIcon(swatch), tr("Trim %1  %2 – %3").arg(i + 1).arg(formatTime(c.start), formatTime(c.end)));
            item->setData(Qt::UserRole, c.id);
            item->setToolTip(tr("Cuts out %1").arg(formatTime(c.end - c.start)));
            m_cutListView->addItem(item);
            if (c.id == m_selectedCut)
                m_cutListView->setCurrentItem(item);
        }
    }
    m_cutListView->setVisible(!cuts.isEmpty());
    m_cutHint->setVisible(cuts.isEmpty());

    {
        const Cut *selected = m_cuts.find(m_selectedCut);
        const QSignalBlocker blockStart(m_startEdit), blockEnd(m_endEdit);
        m_startEdit->setTime(toTime(selected ? selected->start : 0));
        m_endEdit->setTime(toTime(selected ? selected->end : 0));
    }
    m_summaryLabel->setText(m_duration > 0 ? tr("Keeps %1 of %2").arg(formatTime(m_duration - m_cuts.removed()),
                                                                     formatTime(m_duration))
                                           : QString());
    updateControls();
}

void MainWindow::selectCut(int id, bool seekToIt)
{
    m_selectedCut = id;
    refreshCuts();
    if (const Cut *c = seekToIt ? m_cuts.find(id) : nullptr)
        seek(c->start);
}

void MainWindow::addCut(qint64 start, qint64 end)
{
    const int id = m_cuts.add(start, end);
    if (!id)
        m_status->setText(tr("That section is too short to trim (the minimum is %1 ms).").arg(CutList::kMinLength));
    selectCut(id ? id : m_selectedCut, false);
}

void MainWindow::markTrimStart()
{
    if (m_duration <= 0)
        return;
    m_mark = currentPosition();
    m_status->setText(tr("Trimming from %1. Go to where the section ends and press O (Trim to here).")
                          .arg(formatTime(m_mark)));
    refreshCuts();
}

void MainWindow::markTrimEnd()
{
    if (m_duration <= 0)
        return;
    const qint64 position = currentPosition();
    const qint64 from = qMax<qint64>(0, m_mark);
    m_mark = -1;
    m_status->clear();
    addCut(qMin(from, position), qMax(from, position));
}

void MainWindow::removeSelectedCut()
{
    m_cuts.remove(m_selectedCut);
    selectCut(0, false);
}

void MainWindow::showPointer(const QPoint &videoPos, bool overVideo, const QPointF &globalPos)
{
    const QSize video = m_view->videoSize();
    const QString where = overVideo ? QStringLiteral("%1, %2").arg(videoPos.x()).arg(videoPos.y())
                                    : QStringLiteral("–");
    QString text = tr("Pointer   video %1 of %2×%3").arg(where).arg(video.width()).arg(video.height());

    if (const QScreen *s = screen()) {
        const qreal dpr = s->devicePixelRatio();
        const QSize resolution = (QSizeF(s->size()) * dpr).toSize();
        if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
            // Wayland does not tell clients where on the screen they are, so
            // the best available is the position inside this window.
            const QPoint p = (mapFromGlobal(globalPos) * dpr).toPoint();
            text += tr("   window %1, %2 (screen %3×%4)")
                        .arg(p.x())
                        .arg(p.y())
                        .arg(resolution.width())
                        .arg(resolution.height());
        } else {
            const QPoint p = ((globalPos - QPointF(s->geometry().topLeft())) * dpr).toPoint();
            text += tr("   screen %1, %2 of %3×%4")
                        .arg(p.x())
                        .arg(p.y())
                        .arg(resolution.width())
                        .arg(resolution.height());
        }
    }
    m_pointerLabel->setText(text);
}

void MainWindow::showCrop(const QRect &crop)
{
    if (crop.isEmpty()) {
        m_cropLabel->setText(hasMedia() ? tr("Crop   none – drag on the video") : QString());
    } else {
        m_cropLabel->setText(tr("Crop   %1×%2 at %3, %4 (to %5, %6)")
                                 .arg(crop.width())
                                 .arg(crop.height())
                                 .arg(crop.x())
                                 .arg(crop.y())
                                 .arg(crop.x() + crop.width())
                                 .arg(crop.y() + crop.height()));
    }
    updateControls();
}

void MainWindow::startExport()
{
    if (m_exporter || !hasMedia() || m_duration <= 0)
        return;

    const QFileInfo in(m_path);
    const bool cropped = m_view->hasCrop();
    const bool trimmed = !m_cuts.isEmpty();
    const QList<KeepRange> keep = m_cuts.kept();
    if (keep.isEmpty()) {
        QMessageBox::warning(this, tr("Export video"), tr("The trims cut out the whole video; there is nothing left to export."));
        return;
    }
    QString extension = in.suffix().toLower();
    // Containers that take H.264 or H.265 plus the usual audio codecs;
    // anything else goes to Matroska, which accepts every codec.
    if (!QStringList{"mp4", "m4v", "mov", "mkv"}.contains(extension))
        extension = QStringLiteral("mkv");
    const QString suffix = cropped && trimmed ? QStringLiteral("_edited")
                           : cropped          ? QStringLiteral("_cropped")
                           : trimmed          ? QStringLiteral("_trimmed")
                           : m_reencodeCheck->isChecked() ? QStringLiteral("_reencoded")
                                                          : QStringLiteral("_copy");
    QString out = QFileDialog::getSaveFileName(
        this, tr("Export video"), in.dir().filePath(in.completeBaseName() + suffix + QLatin1Char('.') + extension),
        tr("Video (*.mp4 *.m4v *.mov *.mkv)"));
    if (out.isEmpty())
        return;
    if (QFileInfo(out).suffix().isEmpty())
        out += QLatin1Char('.') + extension;
    if (QFileInfo(out) == in) {
        QMessageBox::warning(this, tr("Export video"), tr("Choose a different name: the export can't replace the video it is reading."));
        return;
    }

    m_player->pause();

    ExportJob job;
    job.input = m_path;
    job.output = out;
    job.keep = keep;
    job.crop = m_view->crop();
    job.displaySize = m_view->videoSize();
    job.reencode = m_reencodeCheck->isChecked();
    job.preset = ExportPreset(m_presetCombo->currentData().toInt());
    job.hardware = m_hardwareCheck->isChecked();

    m_exporter = new Exporter(job, this);
    connect(m_exporter, &Exporter::progress, this, [this](double f) { m_progress->setValue(int(f * 1000)); });
    connect(m_exporter, &Exporter::succeeded, this, [this, out](const QString &details) {
        const QFileInfo info(out);
        m_status->setText(tr("Exported <a href=\"%1\">%2</a> (%3) in %4 s, %5. <a href=\"%6\">Show folder</a>")
                              .arg(QUrl::fromLocalFile(out).toString(), info.fileName().toHtmlEscaped(),
                                   QLocale().formattedDataSize(info.size()),
                                   QString::number(m_exportClock.elapsed() / 1000.0, 'f', 1), details.toHtmlEscaped(),
                                   QUrl::fromLocalFile(info.absolutePath()).toString()));
        setExporting(false);
    });
    connect(m_exporter, &Exporter::failed, this, [this](const QString &error) {
        m_status->setText(tr("Export failed."));
        setExporting(false);
        QMessageBox::critical(this, tr("Export failed"), error);
    });
    connect(m_exporter, &Exporter::cancelled, this, [this] {
        m_status->setText(tr("Export cancelled."));
        setExporting(false);
    });
    connect(m_exporter, &QThread::finished, m_exporter, &QObject::deleteLater);

    m_status->setText(tr("Exporting %1…").arg(QFileInfo(out).fileName().toHtmlEscaped()));
    m_progress->setValue(0);
    m_exportClock.start();
    m_exporter->start();
    setExporting(true);
}

void MainWindow::setExporting(bool exporting)
{
    if (!exporting)
        m_exporter = nullptr;
    m_progress->setVisible(exporting);
    m_cancelButton->setVisible(exporting);
    updateControls();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_exporter) {
        m_exporter->cancel();
        m_exporter->wait();
    }
    stopPreviewCopy();
    m_player->stop();
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls() && event->mimeData()->urls().constFirst().isLocalFile())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    if (!urls.isEmpty() && urls.constFirst().isLocalFile()) {
        openFile(urls.constFirst().toLocalFile());
        event->acceptProposedAction();
    }
}
