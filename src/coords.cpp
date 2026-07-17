#include "coords.h"

#include <algorithm>
#include <cmath>

VideoLayout computeLayout(int viewW, int viewH, int videoW, int videoH)
{
    VideoLayout lo;
    lo.vw = videoW;
    lo.vh = videoH;
    if (viewW <= 0 || viewH <= 0 || videoW <= 0 || videoH <= 0)
        return lo;

    lo.scale = std::min(static_cast<float>(viewW) / videoW,
                        static_cast<float>(viewH) / videoH);
    lo.dst.w = videoW * lo.scale;
    lo.dst.h = videoH * lo.scale;
    lo.dst.x = (viewW - lo.dst.w) / 2.0f;
    lo.dst.y = (viewH - lo.dst.h) / 2.0f;
    return lo;
}

bool windowToVideo(const VideoLayout& lo, float wx, float wy, int& vx, int& vy)
{
    if (lo.scale <= 0.0f || lo.vw <= 0 || lo.vh <= 0) {
        vx = 0;
        vy = 0;
        return false;
    }
    const float fx = (wx - lo.dst.x) / lo.scale;
    const float fy = (wy - lo.dst.y) / lo.scale;
    vx = std::clamp(static_cast<int>(std::floor(fx)), 0, lo.vw - 1);
    vy = std::clamp(static_cast<int>(std::floor(fy)), 0, lo.vh - 1);
    return fx >= 0.0f && fx < lo.vw && fy >= 0.0f && fy < lo.vh;
}

SDL_FRect videoRectToWindow(const VideoLayout& lo, const CropRect& r)
{
    SDL_FRect out;
    out.x = lo.dst.x + r.x * lo.scale;
    out.y = lo.dst.y + r.y * lo.scale;
    out.w = r.w * lo.scale;
    out.h = r.h * lo.scale;
    return out;
}

CropRect normalizeRect(int ax, int ay, int cx, int cy)
{
    CropRect r;
    r.x = std::min(ax, cx);
    r.y = std::min(ay, cy);
    r.w = std::abs(cx - ax) + 1;
    r.h = std::abs(cy - ay) + 1;
    return r;
}

CropRect makeEncodableRect(CropRect r, int vw, int vh)
{
    const auto fitAxis = [](int pos, int len, int limit) {
        pos &= ~1;
        len &= ~1;
        len = std::clamp(len, 2, std::max(2, limit & ~1));
        if (pos + len > limit)
            pos = std::max(0, limit - len) & ~1;
        return std::pair<int, int>(pos, len);
    };
    std::tie(r.x, r.w) = fitAxis(r.x, r.w, vw);
    std::tie(r.y, r.h) = fitAxis(r.y, r.h, vh);
    return r;
}
