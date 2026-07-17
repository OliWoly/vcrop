#pragma once

#include <string>

#include "coords.h"
#include "playback_clock.h"
#include "video_decoder.h"

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;
union SDL_Event;

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
    void doCrop();
    void doCopyCommand();

    bool selectionValid() const;
    CropRect selectionRect() const;

    std::string inputPath_;
    VideoDecoder decoder_;
    PlaybackClock clock_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    VideoLayout layout_{};
    double duration_ = 0.0;

    bool havePending_ = false; // a decoded frame is waiting for its PTS
    double pendingPts_ = 0.0;
    bool atEof_ = false;

    bool dragging_ = false;
    bool hasSelection_ = false;
    int anchorX_ = 0, anchorY_ = 0; // drag anchor, video space
    int curX_ = 0, curY_ = 0;       // drag current point, video space

    float seekUi_ = 0.0f;
    bool scrubbing_ = false;
    bool quitRequested_ = false;
    std::string lastError_;
};
