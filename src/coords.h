#pragma once

#include <SDL.h>

// A crop region in video pixel space.
struct CropRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// Letterbox fit of the video into a viewport, recomputed every frame from the
// current window size so resizes never invalidate a selection (selections are
// stored in video space).
struct VideoLayout {
    SDL_FRect dst{};    // on-screen rect of the video, in window coordinates
    float scale = 1.0f; // window pixels per video pixel (uniform)
    int vw = 0;
    int vh = 0;
};

VideoLayout computeLayout(int viewW, int viewH, int videoW, int videoH);

// Maps a window-space point to video pixel coordinates. The output is always
// clamped to [0, vw-1] x [0, vh-1] so drags past the edge stick to the border;
// the return value reports whether the point was actually inside the video.
bool windowToVideo(const VideoLayout& lo, float wx, float wy, int& vx, int& vy);

SDL_FRect videoRectToWindow(const VideoLayout& lo, const CropRect& r);

// Builds a rect from a drag anchor and current point (video space), valid for
// drags in any direction. Endpoints are inclusive, so w/h are always >= 1.
CropRect normalizeRect(int ax, int ay, int cx, int cy);

// Rounds a rect to even x/y/w/h (yuv420 encoders reject odd dimensions and
// even offsets keep chroma siting clean) and clamps it inside the video.
CropRect makeEncodableRect(CropRect r, int vw, int vh);
