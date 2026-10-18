#pragma once

#include "Cuts.h"

#include <QElapsedTimer>
#include <QMediaPlayer>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <memory>

class Exporter;
class QAudioOutput;
class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QTemporaryDir;
class QPushButton;
class QTimeEdit;
class QToolButton;
class TrimSlider;
class VideoView;

// One window per video: preview with crop overlay, the list of trims beside
// it, the timeline, transport controls and the export action.
class MainWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    bool hasMedia() const { return !m_path.isEmpty(); }
    void openFile(const QString &path);

protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void buildUi();
    void bindShortcuts();
    void updateControls();

    void chooseFile();
    void togglePlay();
    qint64 currentPosition() const;
    void seek(qint64 ms);
    void flushSeek();
    void showPosition(qint64 ms);
    void setPlayerPosition(qint64 ms);
    void primeSeeking();
    void finishPriming();
    void step(qint64 deltaMs);
    qint64 frameMs() const;

    // Trims: sections that are cut out of the video.
    void refreshCuts();
    void selectCut(int id, bool seekToIt);
    void addCut(qint64 start, qint64 end);
    void markTrimStart();
    void markTrimEnd();
    void removeSelectedCut();

    void onDurationChanged(qint64 ms);
    void onPositionChanged(qint64 ms);
    void onMediaStatusChanged(QMediaPlayer::MediaStatus status);
    void showPointer(const QPoint &videoPos, bool overVideo, const QPointF &globalPos);
    void showCrop(const QRect &crop);

    void startPreviewCopy();
    void stopPreviewCopy();
    void startExport();
    void setExporting(bool exporting);

    QMediaPlayer *m_player;
    QAudioOutput *m_audio;
    VideoView *m_view;
    TrimSlider *m_timeline;

    QLabel *m_pointerLabel;
    QLabel *m_cropLabel;
    QToolButton *m_playButton;
    QToolButton *m_muteButton;
    QLabel *m_timeLabel;
    QTimeEdit *m_startEdit;
    QTimeEdit *m_endEdit;
    QPushButton *m_markStartButton;
    QPushButton *m_markEndButton;
    QLabel *m_summaryLabel;
    QListWidget *m_cutListView;
    QLabel *m_cutHint;
    QPushButton *m_removeCutButton;
    QPushButton *m_clearCutsButton;
    QPushButton *m_resetCropButton;
    QCheckBox *m_reencodeCheck;
    QComboBox *m_presetCombo;
    QCheckBox *m_hardwareCheck;
    QPushButton *m_exportButton;
    QLabel *m_status;
    QProgressBar *m_progress;
    QPushButton *m_cancelButton;

    QString m_path;
    qint64 m_duration = 0;
    qint64 m_lastPosition = 0; // the playhead as the window knows it
    // Paused after a seek: the requested position stands, however the player
    // rounds it when it reports back.
    bool m_positionHeld = false;
    // macOS: the player is being run for an instant after loading, see primeSeeking().
    bool m_priming = false;
    CutList m_cuts;
    int m_selectedCut = 0; // id of the trim the Start / End fields edit, 0 for none
    qint64 m_mark = -1;    // where "Trim from here" was pressed, waiting for its end
    // The user's own choice for the Re-encode box, which is forced on (and
    // shown ticked) for as long as there is a crop.
    bool m_reencodeWanted = false;

    // Scrubbing produces far more seeks than the player can serve, so only
    // the most recent one is forwarded, a few times per second.
    QTimer m_seekTimer;
    qint64 m_pendingSeek = 0;
    qint64 m_sentSeek = -1;

    // Some players cannot read every container (AVFoundation and Matroska,
    // for one). When loading fails, the streams are copied into a temporary
    // MOV and that is played instead; exports still read the original.
    std::unique_ptr<QTemporaryDir> m_previewDir;
    QPointer<Exporter> m_previewJob;
    bool m_previewTried = false;

    QPointer<Exporter> m_exporter;
    QElapsedTimer m_exportClock;
};
