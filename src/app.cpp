#include "app.h"

#include <SDL.h>
#include <algorithm>
#include <cstdio>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

#include "crop_command.h"

extern "C" {
#include <libavutil/frame.h>
}

#include "icon_data.h"
#include "stb_image.h"

namespace {

void applyWindowIcon(SDL_Window* window)
{
    int w = 0, h = 0, n = 0;
    unsigned char* pixels =
        stbi_load_from_memory(kIconPng, kIconPngLen, &w, &h, &n, 4);
    if (pixels == nullptr)
        return;
    SDL_Surface* icon = SDL_CreateRGBSurfaceWithFormatFrom(
        pixels, w, h, 32, w * 4, SDL_PIXELFORMAT_RGBA32);
    if (icon != nullptr) {
        SDL_SetWindowIcon(window, icon);
        SDL_FreeSurface(icon);
    }
    stbi_image_free(pixels);
}

std::string formatTime(double t)
{
    const int total = static_cast<int>(std::max(0.0, t));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d:%02d", total / 60, total % 60);
    return buf;
}

std::string formatTimePrecise(double t)
{
    t = std::max(0.0, t);
    const int minutes = static_cast<int>(t) / 60;
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%d:%04.1f", minutes, t - minutes * 60);
    return buf;
}

} // namespace

int App::run(const std::string& path)
{
    inputPath_ = path;
    std::string err;
    if (!initVideoAndWindow(err)) {
        std::fprintf(stderr, "vcrop: error: %s\n", err.c_str());
        shutdown();
        return 1;
    }

    while (!quitRequested_) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            handleEvent(e);
        }
        updatePlayback();

        int w = 0, h = 0;
        SDL_GetRendererOutputSize(renderer_, &w, &h);
        layout_ = computeLayout(w, h, decoder_.width(), decoder_.height());

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        drawUi();
        ImGui::Render();

        SDL_SetRenderDrawColor(renderer_, 18, 18, 18, 255);
        SDL_RenderClear(renderer_);
        drawVideoAndOverlay();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer_);
        SDL_RenderPresent(renderer_);
    }

    shutdown();
    return 0;
}

bool App::initVideoAndWindow(std::string& err)
{
    if (!decoder_.open(inputPath_, err))
        return false;
    duration_ = decoder_.durationSec();

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        err = std::string("SDL init failed: ") + SDL_GetError();
        return false;
    }

    // Fit the window to the video, capped to the usable display area.
    const int vw = decoder_.width();
    const int vh = decoder_.height();
    SDL_Rect usable{0, 0, 1280, 720};
    SDL_GetDisplayUsableBounds(0, &usable);
    const float fit = std::min({1.0f,
                                static_cast<float>(usable.w - 80) / vw,
                                static_cast<float>(usable.h - 120) / vh});
    const int winW = std::max(480, static_cast<int>(vw * fit));
    const int winH = std::max(360, static_cast<int>(vh * fit));

    const std::string title = "vcrop - " + inputPath_;
    window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, winW, winH,
                               SDL_WINDOW_RESIZABLE);
    if (window_ == nullptr) {
        err = std::string("cannot create window: ") + SDL_GetError();
        return false;
    }
    applyWindowIcon(window_);
    renderer_ = SDL_CreateRenderer(
        window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer_ == nullptr) {
        err = std::string("cannot create renderer: ") + SDL_GetError();
        return false;
    }
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_IYUV,
                                 SDL_TEXTUREACCESS_STREAMING, vw, vh);
    if (texture_ == nullptr) {
        err = std::string("cannot create video texture: ") + SDL_GetError();
        return false;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // no imgui.ini litter
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window_, renderer_);
    ImGui_ImplSDLRenderer2_Init(renderer_);

    // Show the first frame immediately, then start playing with one frame
    // decoded ahead.
    if (decoder_.nextFrame() != VideoDecoder::Result::Frame) {
        err = "cannot decode the first video frame";
        return false;
    }
    uploadFrame();
    clock_.seekTo(decoder_.framePtsSec());
    clock_.play();
    advancePending();
    return true;
}

void App::shutdown()
{
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
    }
    if (texture_ != nullptr)
        SDL_DestroyTexture(texture_);
    if (renderer_ != nullptr)
        SDL_DestroyRenderer(renderer_);
    if (window_ != nullptr)
        SDL_DestroyWindow(window_);
    SDL_Quit();
}

void App::handleEvent(const SDL_Event& e)
{
    const ImGuiIO& io = ImGui::GetIO();
    switch (e.type) {
    case SDL_QUIT:
        quitRequested_ = true;
        break;
    case SDL_KEYDOWN:
        if (io.WantCaptureKeyboard)
            break;
        if (e.key.keysym.sym == SDLK_ESCAPE)
            quitRequested_ = true;
        else if (e.key.keysym.sym == SDLK_SPACE)
            togglePlay();
        else if (e.key.keysym.sym == SDLK_i)
            trimStart_ = currentTimeClamped();
        else if (e.key.keysym.sym == SDLK_o)
            trimEnd_ = currentTimeClamped();
        break;
    case SDL_MOUSEBUTTONDOWN:
        if (e.button.button == SDL_BUTTON_LEFT && !io.WantCaptureMouse) {
            int vx = 0, vy = 0;
            if (windowToVideo(layout_, static_cast<float>(e.button.x),
                              static_cast<float>(e.button.y), vx, vy)) {
                dragging_ = true;
                hasSelection_ = true;
                anchorX_ = curX_ = vx;
                anchorY_ = curY_ = vy;
            }
        }
        break;
    case SDL_MOUSEMOTION:
        if (dragging_) {
            windowToVideo(layout_, static_cast<float>(e.motion.x),
                          static_cast<float>(e.motion.y), curX_, curY_);
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if (e.button.button == SDL_BUTTON_LEFT)
            dragging_ = false;
        break;
    default:
        break;
    }
}

void App::togglePlay()
{
    if (clock_.playing()) {
        clock_.pause();
    } else {
        if (atEof_)
            doSeek(0.0);
        clock_.play();
    }
}

void App::updatePlayback()
{
    if (!clock_.playing())
        return;
    // Present every frame whose PTS has come due, capped so a stall cannot
    // trigger a decode death spiral.
    int catchup = 0;
    while (havePending_ && clock_.now() >= pendingPts_ && catchup < 4) {
        uploadFrame();
        advancePending();
        ++catchup;
    }
    // If we are far behind (e.g. the window was blocked), snap instead of chasing.
    if (havePending_ && clock_.now() - pendingPts_ > 1.0)
        clock_.seekTo(pendingPts_);
}

void App::advancePending()
{
    switch (decoder_.nextFrame()) {
    case VideoDecoder::Result::Frame:
        havePending_ = true;
        pendingPts_ = decoder_.framePtsSec();
        break;
    case VideoDecoder::Result::Eof:
        havePending_ = false;
        atEof_ = true;
        clock_.pause();
        if (duration_ > 0.0)
            clock_.seekTo(duration_);
        break;
    case VideoDecoder::Result::Error:
        havePending_ = false;
        clock_.pause();
        lastError_ = "decode error; playback stopped";
        break;
    }
}

void App::uploadFrame()
{
    const AVFrame* f = decoder_.frame();
    SDL_UpdateYUVTexture(texture_, nullptr, f->data[0], f->linesize[0],
                         f->data[1], f->linesize[1], f->data[2], f->linesize[2]);
}

void App::doSeek(double t)
{
    if (!decoder_.seek(t)) {
        lastError_ = "seek failed";
        return;
    }
    uploadFrame();
    clock_.seekTo(decoder_.framePtsSec());
    atEof_ = false;
    advancePending();
}

void App::drawVideoAndOverlay()
{
    SDL_RenderCopyF(renderer_, texture_, nullptr, &layout_.dst);
    if (!hasSelection_)
        return;

    const SDL_FRect sel = videoRectToWindow(layout_, selectionRect());
    const SDL_FRect& dst = layout_.dst;

    // Dim everything outside the selection.
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 130);
    const SDL_FRect shades[4] = {
        {dst.x, dst.y, dst.w, sel.y - dst.y},                                 // top
        {dst.x, sel.y + sel.h, dst.w, dst.y + dst.h - (sel.y + sel.h)},       // bottom
        {dst.x, sel.y, sel.x - dst.x, sel.h},                                 // left
        {sel.x + sel.w, sel.y, dst.x + dst.w - (sel.x + sel.w), sel.h},       // right
    };
    for (const SDL_FRect& s : shades) {
        if (s.w > 0.0f && s.h > 0.0f)
            SDL_RenderFillRectF(renderer_, &s);
    }

    SDL_SetRenderDrawColor(renderer_, 90, 220, 130, 255);
    SDL_RenderDrawRectF(renderer_, &sel);
    const SDL_FRect inner{sel.x + 1, sel.y + 1, sel.w - 2, sel.h - 2};
    if (inner.w > 0.0f && inner.h > 0.0f)
        SDL_RenderDrawRectF(renderer_, &inner);
}

void App::drawUi()
{
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::Begin("vcrop", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    int vx = 0, vy = 0;
    const bool hovering = !ImGui::GetIO().WantCaptureMouse &&
                          windowToVideo(layout_, static_cast<float>(mx),
                                        static_cast<float>(my), vx, vy);
    if (hovering)
        ImGui::Text("Cursor: %4d, %4d", vx, vy);
    else
        ImGui::TextDisabled("Cursor: outside video");

    if (selectionValid()) {
        const CropRect r = selectionRect();
        ImGui::Text("Crop:   x=%d  y=%d  w=%d  h=%d", r.x, r.y, r.w, r.h);
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) {
            hasSelection_ = false;
            dragging_ = false;
        }
    } else {
        ImGui::TextDisabled("Crop:   drag a rectangle on the video");
    }

    ImGui::Separator();
    if (ImGui::Button(clock_.playing() ? "Pause" : "Play"))
        togglePlay();
    ImGui::SameLine();
    const double shown = currentTimeClamped();
    ImGui::Text("%s / %s", formatTime(shown).c_str(), formatTime(duration_).c_str());

    if (duration_ > 0.0) {
        if (!scrubbing_)
            seekUi_ = static_cast<float>(shown);
        ImGui::SetNextItemWidth(260.0f);
        ImGui::SliderFloat("##seek", &seekUi_, 0.0f,
                           static_cast<float>(duration_), "");
        if (ImGui::IsItemActive())
            scrubbing_ = true;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            doSeek(static_cast<double>(seekUi_));
            scrubbing_ = false;
        } else if (scrubbing_ && !ImGui::IsItemActive()) {
            scrubbing_ = false;
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Set start"))
        trimStart_ = shown;
    ImGui::SameLine();
    if (ImGui::Button("Set end"))
        trimEnd_ = shown;
    ImGui::SameLine();
    if (trimActive()) {
        const std::string from =
            trimStart_ >= 0.0 ? formatTimePrecise(trimStart_) : "start";
        const std::string to = trimEnd_ >= 0.0 ? formatTimePrecise(trimEnd_) : "end";
        ImGui::Text("%s - %s", from.c_str(), to.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            trimStart_ = -1.0;
            trimEnd_ = -1.0;
        }
    } else {
        ImGui::TextDisabled("no trim (i/o set start/end)");
    }
    if (!trimRangeOk())
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                           "trim end must be after trim start");

    ImGui::Separator();
    ImGui::BeginDisabled(!actionReady());
    if (ImGui::Button(actionLabel()))
        doCrop();
    ImGui::SameLine();
    if (ImGui::Button("Copy command"))
        doCopyCommand();
    ImGui::EndDisabled();

    if (!lastError_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                           lastError_.c_str());
    ImGui::End();
}

bool App::selectionValid() const
{
    if (!hasSelection_)
        return false;
    const CropRect raw = normalizeRect(anchorX_, anchorY_, curX_, curY_);
    return raw.w >= 2 && raw.h >= 2;
}

CropRect App::selectionRect() const
{
    return makeEncodableRect(normalizeRect(anchorX_, anchorY_, curX_, curY_),
                             decoder_.width(), decoder_.height());
}

bool App::trimRangeOk() const
{
    if (trimStart_ >= 0.0 && trimEnd_ >= 0.0)
        return trimEnd_ > trimStart_;
    return true;
}

bool App::actionReady() const
{
    return (selectionValid() || trimActive()) && trimRangeOk();
}

const char* App::actionLabel() const
{
    const bool crop = selectionValid();
    const bool trim = trimActive();
    if (crop && trim)
        return "Crop + Trim";
    if (trim)
        return "Trim";
    return "Crop";
}

CropJob App::buildJob() const
{
    CropJob job;
    job.inputPath = inputPath_;
    job.hasCrop = selectionValid();
    if (job.hasCrop)
        job.crop = selectionRect();
    job.trimStart = trimStart_;
    job.trimEnd = trimEnd_;
    return job;
}

double App::currentTimeClamped() const
{
    const double now = std::max(0.0, clock_.now());
    return duration_ > 0.0 ? std::min(now, duration_) : now;
}

void App::doCrop()
{
    const CropJob job = buildJob();
    const std::string out = buildOutputPath(job);
    const auto argv = buildFfmpegArgv(job, out);
    std::string err;
    if (!spawnDetached(argv, err)) {
        lastError_ = "failed to start ffmpeg: " + err;
        return;
    }
    std::printf("vcrop: processing in background -> %s\n", out.c_str());
    quitRequested_ = true;
}

void App::doCopyCommand()
{
    const CropJob job = buildJob();
    const auto argv = buildFfmpegArgv(job, buildOutputPath(job));
    const std::string cmd = buildShellCommand(argv);
    SDL_SetClipboardText(cmd.c_str());
    // Also print the literal command to stdout as a fallback: on X11/Wayland
    // the clipboard may not outlive the process unless a clipboard manager is
    // running. The notice goes to stderr so stdout stays pipeable.
    std::fprintf(stderr, "vcrop: command copied to clipboard:\n");
    std::printf("%s\n", cmd.c_str());
    quitRequested_ = true;
}
