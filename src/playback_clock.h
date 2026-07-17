#pragma once

#include <chrono>

// Maps wall-clock time to media time. The wall clock is the playback master:
// frames are presented when their PTS falls due against now().
class PlaybackClock {
public:
    bool playing() const { return playing_; }

    double now() const
    {
        if (!playing_)
            return mediaBase_;
        const std::chrono::duration<double> elapsed =
            std::chrono::steady_clock::now() - wallBase_;
        return mediaBase_ + elapsed.count();
    }

    void play()
    {
        if (playing_)
            return;
        wallBase_ = std::chrono::steady_clock::now();
        playing_ = true;
    }

    void pause()
    {
        if (!playing_)
            return;
        mediaBase_ = now();
        playing_ = false;
    }

    // Rebase to media time t; does not change the playing state.
    void seekTo(double t)
    {
        mediaBase_ = t;
        wallBase_ = std::chrono::steady_clock::now();
    }

private:
    bool playing_ = false;
    double mediaBase_ = 0.0;
    std::chrono::steady_clock::time_point wallBase_{};
};
