#pragma once

#include <string>

#include <SDL.h>

#include "audio_decoder.h"
#include "coords.h"
#include "crop_command.h"
#include "playback_clock.h"
#include "video_decoder.h"

class App {
public:
    // Runs the full lifecycle for one input file; returns the process exit code.
    int run(const std::string& path);

private:
    bool initVideoAndWindow(std::string& err);
    void shutdown();
    void handleEvent(const SDL_Event& e);
    void togglePlay();
    void updatePlayback();
    void advancePending();
    void uploadFrame();
    void doSeek(double t);
    void drawVideoAndOverlay();
    void drawUi();
    void drawHoverOverlay(int vx, int vy, bool hovering);
    void copyHoveredCoords(int vx, int vy);
    bool copyFeedbackActive() const;
    void doCrop();
    void doCopyCommand();

    // Decodes one audio chunk and queues it on the audio device at the
    // current volume; returns false when the audio stream is exhausted.
    bool queueAudioChunk();
    double audioQueuedAheadSec() const;
    double audioPlaybackSec() const;
    bool audioDone() const;

    bool selectionValid() const;
    CropRect selectionRect() const;
    bool trimActive() const { return trimStart_ >= 0.0 || trimEnd_ >= 0.0; }
    bool trimRangeOk() const;
    bool actionReady() const;
    const char* actionLabel() const;
    CropJob buildJob() const;
    double currentTimeClamped() const;

    std::string inputPath_;
    VideoDecoder decoder_;
    AudioDecoder audio_;
    PlaybackClock clock_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    VideoLayout layout_{};
    double duration_ = 0.0;

    // Audio playback state; audioDev_ == 0 means the file has no usable
    // audio stream and playback falls back to the wall clock.
    SDL_AudioDeviceID audioDev_ = 0;
    double audioQueuedSec_ = 0.0; // seconds of audio queued since last seek
    double audioBaseSec_ = 0.0;   // media time of the oldest queued sample
    bool audioBaseSet_ = false;
    bool audioEof_ = false;
    bool muted_ = false;
    float volume_ = 1.0f;

    bool havePending_ = false; // a decoded frame is waiting for its PTS
    double pendingPts_ = 0.0;
    bool atEof_ = false;

    double trimStart_ = -1.0; // seconds; negative = unset
    double trimEnd_ = -1.0;

    bool dragging_ = false;
    bool hasSelection_ = false;
    int anchorX_ = 0, anchorY_ = 0; // drag anchor, video space
    int curX_ = 0, curY_ = 0;       // drag current point, video space

    std::string copyFeedback_;
    uint32_t copyFeedbackUntil_ = 0; // SDL_GetTicks() deadline; 0 = inactive

    float seekUi_ = 0.0f;
    bool scrubbing_ = false;
    bool uiCollapsed_ = false;
    bool quitRequested_ = false;
    std::string lastError_;
};
