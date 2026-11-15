#include "expo.hpp"

#include <algorithm>
#include <any>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cairo/cairo.h>
#include <drm_fourcc.h>
#include <xkbcommon/xkbcommon.h>

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/animation/AnimationManager.hpp>
#include <hyprland/src/managers/animation/DesktopAnimationManager.hpp>
#include <hyprland/src/managers/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#undef private

// Colours: the background and the accent are plugin:jexpo:col_bg / col_accent; text is fixed.
static const CHyprColor TEXT    = CHyprColor{0xe2 / 255.0, 0xe5 / 255.0, 0xdc / 255.0, 1.0};
static const CHyprColor SUBTEXT = CHyprColor{0xa3 / 255.0, 0xa9 / 255.0, 0x9c / 255.0, 1.0};

static CHyprColor bgColor() {
    static auto* const* P = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:jexpo:col_bg")->getDataStaticPtr();
    return CHyprColor((uint64_t)**P);
}
static CHyprColor accentColor() {
    static auto* const* P = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:jexpo:col_accent")->getDataStaticPtr();
    return CHyprColor((uint64_t)**P);
}
static std::string fontName() {
    static auto const* P = (Hyprlang::STRING const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:jexpo:font")->getDataStaticPtr();
    return *P ? std::string{*P} : std::string{"monospace"};
}

static constexpr double MARGIN    = 56;  // around the grid, logical px
static constexpr double GAP       = 30;  // between tiles
static constexpr double HINT      = 40;  // under the grid
static constexpr double ROUNDING  = 10;
static constexpr double BADGE     = 40;  // the number's square, also a handle
static constexpr double THRESHOLD = 6;   // px of movement before a press becomes a drag

class CExpoPassElement : public IPassElement {
  public:
    void draw(const CRegion& damage) override {
        if (g_pExpo)
            g_pExpo->fullRender();
    }
    bool needsLiveBlur() override {
        return false;
    }
    bool needsPrecomputeBlur() override {
        return false;
    }
    std::optional<CBox> boundingBox() override {
        if (!g_pExpo || !g_pExpo->m_monitor)
            return std::nullopt;
        return CBox{{}, g_pExpo->m_monitor->m_size};
    }
    CRegion opaqueRegion() override {
        if (!g_pExpo || !g_pExpo->m_monitor)
            return CRegion{};
        return CBox{{}, g_pExpo->m_monitor->m_size};
    }
    const char* passName() override {
        return "CExpoPassElement";
    }
};

// Hyprland RASSERTs (aborts the whole compositor) on a rect, border or shadow with no area and on
// a texture without a GL name. A zoom near 0 shrinks things to nothing and a framebuffer can fail
// to allocate, so every draw goes through these, which skip what Hyprland would refuse.
static bool drawable(const CBox& px) {
    return px.width >= 1 && px.height >= 1;
}
static void safeRect(const CBox& px, const CHyprColor& c, CHyprOpenGLImpl::SRectRenderData d) {
    if (drawable(px))
        g_pHyprOpenGL->renderRect(px, c, d);
}
static void safeBorder(const CBox& px, const CGradientValueData& g, CHyprOpenGLImpl::SBorderRenderData d) {
    if (drawable(px))
        g_pHyprOpenGL->renderBorder(px, g, d);
}
static void safeShadow(const CBox& px, int round, float power, int range, const CHyprColor& c, float a) {
    if (drawable(px) && range > 0)
        g_pHyprOpenGL->renderRoundedShadow(px, round, power, range, c, a);
}
static void safeTexture(SP<CTexture> tex, const CBox& px, CHyprOpenGLImpl::STextureRenderData d) {
    if (tex && tex->m_texID > 0 && drawable(px))
        g_pHyprOpenGL->renderTexture(tex, px, d);
}

static double lerp(double a, double b, double k) {
    return a + (b - a) * k;
}

// ---------------------------------------------------------------- geometry

Vector2D CExpo::tileSize() const {
    const auto M  = m_monitor->m_size;
    const auto W  = M.x - 2 * MARGIN;
    const auto H  = M.y - 2 * MARGIN - HINT;
    const auto tw = std::min((W - GAP * (COLUMNS - 1)) / COLUMNS, (H - GAP * (COLUMNS - 1)) / COLUMNS * M.x / M.y);
    return {tw, tw * M.y / M.x};
}

double CExpo::tileScale() const {
    return tileSize().x / m_monitor->m_size.x;
}

Vector2D CExpo::slotPos(int slot) const {
    const auto M  = m_monitor->m_size;
    const auto T  = tileSize();
    const auto x0 = (M.x - (T.x * COLUMNS + GAP * (COLUMNS - 1))) / 2;
    const auto y0 = (M.y - HINT - (T.y * COLUMNS + GAP * (COLUMNS - 1))) / 2;
    return {x0 + (slot % COLUMNS) * (T.x + GAP), y0 + (slot / COLUMNS) * (T.y + GAP)};
}

int CExpo::slotAt(const Vector2D& p) const {
    const auto T = tileSize();
    for (int s = 0; s < SLOTS; ++s) {
        const auto P = slotPos(s);
        if (p.x >= P.x - GAP / 2 && p.x <= P.x + T.x + GAP / 2 && p.y >= P.y - GAP / 2 && p.y <= P.y + T.y + GAP / 2)
            return s;
    }
    return -1;
}

int CExpo::tileAt(const Vector2D& p) const {
    const auto T = tileSize();
    for (int t = 0; t < SLOTS; ++t) {
        const auto P = m_tiles[t].pos;
        if (p.x >= P.x && p.x <= P.x + T.x && p.y >= P.y && p.y <= P.y + T.y)
            return t;
    }
    return -1;
}

// A tile's box on screen: at zoom 1 where it is, at 0 scaled so m_zoomSlot fills the monitor.
CBox CExpo::zoomedBox(const Vector2D& pos) const {
    const auto T = tileSize();
    const auto Z = m_zoom->value();
    if (m_zoomSlot < 0)
        return CBox{pos, T};
    const auto ZP = slotPos(m_zoomSlot);
    const auto K  = m_monitor->m_size.x / T.x;
    const auto x0 = (pos.x - ZP.x) * K, y0 = (pos.y - ZP.y) * K;
    return CBox{lerp(x0, pos.x, Z), lerp(y0, pos.y, Z), lerp(T.x * K, T.x, Z), lerp(T.y * K, T.y, Z)};
}

// The topmost window of a tile's workspace under p (logical, monitor-local): floating first.
PHLWINDOW CExpo::windowAt(int tile, const Vector2D& p) const {
    const auto WS = m_tiles[tile].ws.lock();
    if (!WS)
        return nullptr;
    const auto LOCAL = (p - m_tiles[tile].pos) / tileScale() + m_monitor->m_position;
    for (bool floating : {true, false}) {
        for (auto it = g_pCompositor->m_windows.rbegin(); it != g_pCompositor->m_windows.rend(); ++it) {
            const auto& w = *it;
            if (!w->m_isMapped || w->isHidden() || w->m_workspace != WS || w->m_isFloating != floating || w->m_pinned)
                continue;
            if (CBox{w->m_realPosition->goal(), w->m_realSize->goal()}.containsPoint(LOCAL))
                return w;
        }
    }
    return nullptr;
}

std::vector<int> CExpo::provisionalOrder() const {
    auto order = m_order;
    if (m_drag != DRAG_WORKSPACE || m_hoverSlot < 0)
        return order;
    const auto from = std::ranges::find(order, m_pressTile) - order.begin();
    order.erase(order.begin() + from);
    order.insert(order.begin() + m_hoverSlot, m_pressTile);
    return order;
}

static int slotOf(const std::vector<int>& order, int tile) {
    return std::ranges::find(order, tile) - order.begin();
}

// ---------------------------------------------------------------- life

CExpo::CExpo(PHLMONITOR monitor) : m_monitor(monitor) {
    m_startedOn = monitor->m_activeWorkspace;
    m_tiles     = std::vector<STile>(SLOTS);
    for (int i = 0; i < SLOTS; ++i) {
        const auto WS = g_pCompositor->getWorkspaceByID(i + 1);
        if (WS && WS->m_monitor == monitor)
            m_tiles[i].ws = WS;
        m_order.push_back(i);
        m_tiles[i].pos = slotPos(i);
    }
    m_zoomSlot = -1;
    for (int i = 0; i < SLOTS; ++i)
        if (m_tiles[i].ws && m_tiles[i].ws.lock() == m_startedOn.lock())
            m_zoomSlot = i;
    m_selected = std::max(0, m_zoomSlot);

    // the numbers and the hint: drawn once per plugin load and scale (fontconfig is slow to start)
    const auto ACCENT = accentColor();
    if (g_text.scale != monitor->m_scale || g_text.font != fontName() || g_text.accent != ACCENT.getAsHex()) {
        g_pHyprRenderer->makeEGLCurrent();
        g_text        = {};
        g_text.scale  = monitor->m_scale;
        g_text.font   = fontName();
        g_text.accent = ACCENT.getAsHex();
        for (int i = 1; i <= SLOTS; ++i) {
            g_text.number.push_back(textTexture(std::to_string(i), 20, TEXT, g_text.numberSize));
            g_text.numberCurrent.push_back(textTexture(std::to_string(i), 20, ACCENT, g_text.numberSize));
        }
        g_text.hint = textTexture("click to go  ·  drag a window to move it  ·  drag a workspace by its number to reorder  ·  Esc goes back", 14, SUBTEXT, g_text.hintSize);
    }

    // Only the tile the zoom starts from has to be ready for the first frame (it fills the
    // screen); the rest are drawn over the next few frames, a few at a time (onPreRender).
    g_pHyprRenderer->makeEGLCurrent();
    for (int i = 0; i < SLOTS; ++i) {
        if (m_zoomSlot >= 0 && i != m_zoomSlot)
            continue;
        redrawTile(i);
        m_tiles[i].drawn   = true;
        m_tiles[i].drawnAt = std::chrono::steady_clock::now();
    }

    g_pAnimationManager->createAnimation(0.F, m_zoom, g_pConfigManager->getAnimationPropertyConfig("windowsMove"), AVARDAMAGE_NONE);
    m_zoom->setUpdateCallback([](auto) {
        if (g_pExpo)
            g_pExpo->onDamage();
    });
    *m_zoom = 1.F;

    // The tiles are live, so every frame is wanted while it's open. Asking from preRender isn't
    // enough on a real display: the request is dropped while that frame is still pending, and the
    // next one only comes when something else damages the screen (measured: ~100 fps with 50 ms
    // gaps at 164 Hz). A timer at twice the refresh rate asks again; extra asks are no-ops.
    const auto HALF = std::chrono::microseconds((long)(500000.0 / std::max(30.F, monitor->m_refreshRate)));
    m_frameTimer    = makeShared<CEventLoopTimer>(
        HALF,
        [HALF](SP<CEventLoopTimer> self, void*) {
            if (g_pExpo && g_pExpo->m_monitor)
                g_pCompositor->scheduleFrameForMonitor(g_pExpo->m_monitor.lock());
            self->updateTimeout(HALF);
        },
        nullptr);
    g_pEventLoopManager->addTimer(m_frameTimer);

    Cursor::overrideController->setOverride("left_ptr", Cursor::CURSOR_OVERRIDE_UNKNOWN);
    m_mouse     = g_pInputManager->getMouseCoordsInternal() - monitor->m_position;
    m_lastFrame = std::chrono::steady_clock::now();

    m_mouseMoveHook = g_pHookSystem->hookDynamic("mouseMove", [this](void*, SCallbackInfo& info, std::any) {
        info.cancelled = true;
        onMouseMove();
    });
    m_mouseButtonHook = g_pHookSystem->hookDynamic("mouseButton", [this](void*, SCallbackInfo& info, std::any param) {
        info.cancelled = true;
        const auto E   = std::any_cast<IPointer::SButtonEvent>(param);
        onButton(E.button, E.state == WL_POINTER_BUTTON_STATE_PRESSED);
    });
    m_keyHook = g_pHookSystem->hookDynamic("keyPress", [this](void*, SCallbackInfo& info, std::any param) {
        auto       map = std::any_cast<std::unordered_map<std::string, std::any>>(param);
        const auto KB  = std::any_cast<SP<IKeyboard>>(map["keyboard"]);
        const auto E   = std::any_cast<IKeyboard::SKeyEvent>(map["event"]);
        if (!KB || !KB->m_xkbState)
            return;
        if (xkb_state_mod_name_is_active(KB->m_xkbState, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)
            return; // Super+anything stays the session's (Super+Tab closes, Super+1-9 go)
        const auto SYM = xkb_state_key_get_one_sym(KB->m_xkbState, E.keycode + 8);
        if (onKey(SYM, E.state == WL_KEYBOARD_KEY_STATE_PRESSED))
            info.cancelled = true;
    });
}

CExpo::~CExpo() {
    if (const char* dir = getenv("XDG_RUNTIME_DIR"); dir && m_stats.frames > 1) {
        if (FILE* f = fopen((std::string{dir} + "/jexpo-stats").c_str(), "a")) {
            const int N = m_stats.frames;
            fprintf(f, "frames %d  redraw cpu avg %.2f ms max %.2f ms  frame interval avg %.2f ms max %.2f ms\n", N, m_stats.cpuMs / N, m_stats.cpuMaxMs,
                    m_stats.intervalMs / (N - 1), m_stats.intervalMaxMs);
            fclose(f);
        }
    }
    if (m_frameTimer) {
        m_frameTimer->cancel();
        g_pEventLoopManager->removeTimer(m_frameTimer);
    }
    g_pHyprRenderer->makeEGLCurrent();
    for (auto& t : m_tiles)
        t.fb.release();
    m_dragFb.release();
    Cursor::overrideController->unsetOverride(Cursor::CURSOR_OVERRIDE_UNKNOWN);
    if (m_monitor)
        g_pHyprOpenGL->markBlurDirtyForMonitor(m_monitor.lock());
}

// ---------------------------------------------------------------- drawing into the tiles

// Draw a workspace into its tile's framebuffer, as hyprexpo does: make it the monitor's active
// workspace for the length of one render. Surface feedback stays on, so the apps on it get frame
// callbacks and keep drawing: that is what makes every tile live, not only the current one.
void CExpo::redrawTile(int t) {
    const auto MON = m_monitor.lock();
    if (!MON)
        return;
    auto&      tile    = m_tiles[t];
    const auto WS      = tile.ws.lock();
    const auto STARTED = m_startedOn.lock() ? m_startedOn.lock() : MON->m_activeWorkspace;

    m_blockRender = true;
    if (tile.fb.m_size != MON->m_pixelSize) {
        tile.fb.release();
        tile.fb.alloc(MON->m_pixelSize.x, MON->m_pixelSize.y, MON->m_output->state->state().drmFormat);
    }

    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->beginRender(MON, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, &tile.fb);
    g_pHyprOpenGL->clear(CHyprColor{0, 0, 0, 1.0});

    PHLWORKSPACE openSpecial = MON->m_activeSpecialWorkspace;
    if (openSpecial)
        MON->m_activeSpecialWorkspace.reset();
    if (STARTED)
        STARTED->m_visible = false;

    const auto DRAGGED = m_drag == DRAG_WINDOW ? m_pressWindow.lock() : nullptr;
    const auto CBOX    = CBox{{0, 0}, MON->m_pixelSize};
    if (WS) {
        MON->m_activeWorkspace = WS;
        g_pDesktopAnimationManager->startAnimation(WS, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);
        WS->m_visible = true;
        if (DRAGGED && DRAGGED->m_workspace == WS)
            DRAGGED->m_hidden = true;
        g_pHyprRenderer->renderWorkspace(MON, WS, Time::steadyNow(), CBOX);
        // and a frame callback for every surface on it, drawn or not: an app that has never been
        // on screen waits for one before its first frame
        g_pHyprRenderer->sendFrameEventsToWorkspace(MON, WS, Time::steadyNow());
        if (DRAGGED)
            DRAGGED->m_hidden = false;
        WS->m_visible = false;
        g_pDesktopAnimationManager->startAnimation(WS, CDesktopAnimationManager::ANIMATION_TYPE_OUT, false, true);
    } else
        g_pHyprRenderer->renderWorkspace(MON, nullptr, Time::steadyNow(), CBOX); // wallpaper and bar

    g_pHyprOpenGL->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();

    MON->m_activeSpecialWorkspace = openSpecial;
    if (STARTED) {
        MON->m_activeWorkspace = STARTED;
        STARTED->m_visible     = true;
        g_pDesktopAnimationManager->startAnimation(STARTED, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);
    }
    m_blockRender = false;
}

// The window being dragged, alone at its own size, for drawing under the cursor.
void CExpo::redrawDragged() {
    const auto MON = m_monitor.lock();
    const auto W   = m_pressWindow.lock();
    if (!MON || !W || !W->m_workspace)
        return;
    const auto WS = W->m_workspace;
    if (m_dragFb.m_size != MON->m_pixelSize) {
        m_dragFb.release();
        m_dragFb.alloc(MON->m_pixelSize.x, MON->m_pixelSize.y, MON->m_output->state->state().drmFormat);
    }
    m_blockRender         = true;
    const bool WASVISIBLE = WS->m_visible;
    WS->m_visible         = true;
    CRegion fakeDamage{0, 0, INT16_MAX, INT16_MAX};
    g_pHyprRenderer->beginRender(MON, fakeDamage, RENDER_MODE_FULL_FAKE, nullptr, &m_dragFb);
    g_pHyprOpenGL->clear(CHyprColor{0, 0, 0, 0});
    g_pHyprRenderer->renderWindow(W, MON, Time::steadyNow(), true, RENDER_PASS_ALL, true, true);
    g_pHyprOpenGL->m_renderData.blockScreenShader = true;
    g_pHyprRenderer->endRender();
    WS->m_visible = WASVISIBLE;
    m_blockRender = false;
}

void CExpo::onPreRender() {
    const auto MON = m_monitor.lock();
    if (!MON)
        return;

    // Someone else changed the workspace (Super+1-9): zoom into it.
    if (!m_closing && MON->m_activeWorkspace != m_startedOn.lock()) {
        m_startedOn = MON->m_activeWorkspace;
        close(-1);
    }

    const auto NOW = std::chrono::steady_clock::now();
    const auto DT  = std::min(0.05, std::chrono::duration<double>(NOW - m_lastFrame).count());
    m_lastFrame    = NOW;

    // Tiles glide to their slots; the one being dragged stays under the cursor.
    const auto ORDER = provisionalOrder();
    const auto K     = 1.0 - std::exp(-DT * 16.0);
    for (int s = 0; s < SLOTS; ++s) {
        auto& tile = m_tiles[ORDER[s]];
        if (m_drag == DRAG_WORKSPACE && ORDER[s] == m_pressTile)
            tile.pos = m_mouse - m_grabOffset;
        else {
            const auto TARGET = slotPos(s);
            tile.pos          = tile.pos + (TARGET - tile.pos) * K;
            if ((TARGET - tile.pos).size() < 0.5)
                tile.pos = TARGET;
        }
    }

    // Which tiles to draw this frame. Nine full workspace renders every frame is ~1500 a second
    // at 164 Hz, far past a frame's budget with real apps open, and a thumbnail doesn't need it.
    // - the lifted workspace, a window drag's source and, while the zoom runs, the tile it's on
    //   (the one large on screen): every frame
    // - every other tile: at most ONE per frame, the most overdue, so the work spreads evenly
    //   instead of piling several heavy renders onto one frame; a tile never drawn comes first,
    //   then any due by its rate (plugin:jexpo:tile_hz with windows, empty_hz for wallpaper+bar)
    static auto* const* PTILEHZ  = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:jexpo:tile_hz")->getDataStaticPtr();
    static auto* const* PEMPTYHZ = (Hyprlang::INT* const*)HyprlandAPI::getConfigValue(PHANDLE, "plugin:jexpo:empty_hz")->getDataStaticPtr();
    const double        TILE_HZ  = std::max<Hyprlang::INT>(1, **PTILEHZ), EMPTY_HZ = std::max<Hyprlang::INT>(1, **PEMPTYHZ);
    const bool          ZOOMING  = m_zoom->isBeingAnimated();
    g_pHyprRenderer->makeEGLCurrent();
    int    next     = -1;
    double mostLate = 0;
    for (int t = 0; t < SLOTS; ++t) {
        auto&      tile = m_tiles[t];
        const bool EVERY = (m_drag != DRAG_NONE && t == m_pressTile) || (ZOOMING && slotOf(ORDER, t) == m_zoomSlot);
        if (EVERY && tile.drawn) {
            redrawTile(t);
            tile.drawnAt = NOW;
            continue;
        }
        const auto WS     = tile.ws.lock();
        const auto PERIOD = 1.0 / (WS && WS->getWindows() > 0 ? TILE_HZ : EMPTY_HZ);
        const auto LATE   = tile.drawn ? std::chrono::duration<double>(NOW - tile.drawnAt).count() / PERIOD : 1e9; // >= 1: due
        if (LATE >= 1.0 && LATE > mostLate) {
            mostLate = LATE;
            next     = t;
        }
    }
    if (next >= 0) {
        redrawTile(next);
        m_tiles[next].drawnAt = NOW;
        m_tiles[next].drawn   = true;
    }
    const auto CPU = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - NOW).count();
    m_stats.frames++;
    m_stats.cpuMs += CPU;
    m_stats.cpuMaxMs = std::max(m_stats.cpuMaxMs, CPU);
    if (m_stats.frames > 1) {
        m_stats.intervalMs += DT * 1000.0;
        m_stats.intervalMaxMs = std::max(m_stats.intervalMaxMs, DT * 1000.0);
    }
    if (m_drag == DRAG_WINDOW)
        redrawDragged();

    onDamage();
}

void CExpo::onDamage() {
    const auto MON = m_monitor.lock();
    if (!MON)
        return;
    m_blockDamage = true;
    g_pHyprRenderer->damageMonitor(MON);
    m_blockDamage = false;
    g_pCompositor->scheduleFrameForMonitor(MON);
}

void CExpo::render() {
    g_pHyprRenderer->m_renderPass.add(makeUnique<CExpoPassElement>());
}

// ---------------------------------------------------------------- drawing the grid

void CExpo::fullRender() {
    const auto MON   = m_monitor.lock();
    const auto SCALE = MON->m_scale;
    const auto Z     = m_zoom->value();
    const auto FADE  = m_zoomSlot < 0 ? Z : 1.0; // no tile to zoom from: fade in instead
    CRegion    damage{0, 0, INT16_MAX, INT16_MAX};

    const auto BG = bgColor(), ACCENT = accentColor();
    g_pHyprOpenGL->clear(BG.stripA());

    const auto ORDER  = provisionalOrder();
    const auto ACTIVE = MON->m_activeWorkspace;
    auto       drawTile = [&](int s) {
        const int  t   = ORDER[s];
        auto&      tile = m_tiles[t];
        CBox       box  = zoomedBox(tile.pos);
        const auto R    = ROUNDING * (box.w / tileSize().x);
        const bool LIFTED = m_drag == DRAG_WORKSPACE && t == m_pressTile;

        if (LIFTED) {
            CBox shadow = box;
            safeShadow(shadow.scale(SCALE).round(), R * SCALE, 2.F, 30 * SCALE, CHyprColor{0, 0, 0, 0.6}, 1.0);
        }
        CBox px = box;
        px.scale(SCALE).round();
        safeTexture(tile.fb.getTexture(), px, {.damage = &damage, .a = (float)FADE, .round = (int)std::round(R * SCALE)});

        // outline: the drop target, the hovered/selected one, the current workspace
        const auto WS      = tile.ws.lock();
        const bool CURRENT = WS && WS == ACTIVE;
        CHyprColor color   = CHyprColor{0, 0, 0, 0};
        int        width   = 2;
        if (m_drag == DRAG_WINDOW && s == m_hoverSlot) {
            color = ACCENT;
            width = 4;
        } else if (!m_closing && m_drag == DRAG_NONE && s == m_selected)
            color = ACCENT;
        else if (CURRENT)
            color = CHyprColor{ACCENT.r, ACCENT.g, ACCENT.b, 0.45};
        if (color.a > 0 && Z > 0.01) { // renderBorder scales the width itself and draws outside the box
            CBox o = box;
            o.expand(2).scale(SCALE).round();
            safeBorder(o, CGradientValueData{color}, {.round = (int)std::round((R + 2) * SCALE), .borderSize = width, .a = (float)Z});
        }

        // the number, top left: also the handle to drag the workspace by
        const auto ID  = s + 1;
        CBox       bg  = {box.x + 8 * Z, box.y + 8 * Z, BADGE * Z, BADGE * Z};
        const bool HOT = !m_closing && m_drag == DRAG_NONE && tileAt(m_mouse) == t &&
            CBox{tile.pos + Vector2D{8, 8}, Vector2D{BADGE, BADGE}}.containsPoint(m_mouse);
        if (Z > 0.01) {
            CBox b = bg;
            safeRect(b.scale(SCALE).round(), CHyprColor{BG.r, BG.g, BG.b, (HOT ? 0.95 : 0.8) * Z}, {.round = (int)std::round(8 * Z * SCALE)});
            if (HOT) {
                CBox hb = bg;
                safeBorder(hb.scale(SCALE).round(), CGradientValueData{ACCENT}, {.round = (int)std::round(8 * Z * SCALE), .borderSize = 2, .a = (float)Z});
            }
            const auto& TEX = (CURRENT ? g_text.numberCurrent : g_text.number)[ID - 1];
            CBox        tb  = {bg.x + (bg.w - g_text.numberSize.x * Z) / 2, bg.y + (bg.h - g_text.numberSize.y * Z) / 2, g_text.numberSize.x * Z, g_text.numberSize.y * Z};
            safeTexture(TEX, tb.scale(SCALE).round(), {.damage = &damage, .a = (float)Z});
        }
    };

    // the lifted workspace last, so it's on top
    int lifted = -1;
    for (int s = 0; s < SLOTS; ++s) {
        if (m_drag == DRAG_WORKSPACE && ORDER[s] == m_pressTile) {
            lifted = s;
            continue;
        }
        drawTile(s);
    }
    if (lifted >= 0)
        drawTile(lifted);

    // the window being dragged, at the tiles' scale, under the cursor
    if (m_drag == DRAG_WINDOW) {
        const auto W = m_pressWindow.lock();
        if (W) {
            const auto TS  = tileScale();
            const auto SZ  = W->m_realSize->value();
            CBox       box = {m_mouse - m_grabOffset, SZ * TS * 1.04};
            const auto UV  = SZ * SCALE / MON->m_pixelSize;
            CBox       shadow = box;
            safeShadow(shadow.scale(SCALE).round(), ROUNDING * SCALE, 2.F, 24 * SCALE, CHyprColor{0, 0, 0, 0.55}, 1.0);
            g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft     = Vector2D{0, 0};
            g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = UV;
            safeTexture(m_dragFb.getTexture(), box.scale(SCALE).round(), {.damage = &damage, .a = 0.95F, .allowCustomUV = true});
            g_pHyprOpenGL->m_renderData.primarySurfaceUVTopLeft     = Vector2D{-1, -1};
            g_pHyprOpenGL->m_renderData.primarySurfaceUVBottomRight = Vector2D{-1, -1};
        }
    }

    // how to drive it
    if (g_text.hint && Z > 0.01) {
        const auto T  = tileSize();
        const auto Y  = slotPos(SLOTS - 1).y + T.y + (MON->m_size.y - (slotPos(SLOTS - 1).y + T.y) - g_text.hintSize.y) / 2;
        CBox       hb = {(MON->m_size.x - g_text.hintSize.x) / 2, Y, g_text.hintSize.x, g_text.hintSize.y};
        safeTexture(g_text.hint, hb.scale(SCALE).round(), {.damage = &damage, .a = (float)(0.8 * Z)});
    }
}

SP<CTexture> CExpo::textTexture(const std::string& text, int px, const CHyprColor& color, Vector2D& sizeOut) {
    const double SCALE = m_monitor->m_scale;
    const int    FONT  = std::round(px * SCALE);

    // measure, then draw
    auto* probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    auto* pc    = cairo_create(probe);
    cairo_select_font_face(pc, fontName().c_str(), CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(pc, FONT);
    cairo_text_extents_t ext;
    cairo_text_extents(pc, text.c_str(), &ext);
    cairo_font_extents_t fext;
    cairo_font_extents(pc, &fext);
    cairo_destroy(pc);
    cairo_surface_destroy(probe);

    const int W = std::ceil(ext.x_advance) + 4, H = std::ceil(fext.ascent + fext.descent) + 2;
    auto*     surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, H);
    auto*     cr      = cairo_create(surface);
    cairo_select_font_face(cr, fontName().c_str(), CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, FONT);
    cairo_set_source_rgba(cr, color.r, color.g, color.b, color.a);
    cairo_move_to(cr, 2, 1 + fext.ascent);
    cairo_show_text(cr, text.c_str());
    cairo_surface_flush(surface);

    auto tex = makeShared<CTexture>(DRM_FORMAT_ARGB8888, cairo_image_surface_get_data(surface), cairo_image_surface_get_stride(surface), Vector2D{(double)W, (double)H});
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    sizeOut = Vector2D{(double)W, (double)H} / SCALE;
    return tex;
}

// ---------------------------------------------------------------- input

void CExpo::onMouseMove() {
    const auto MON = m_monitor.lock();
    if (!MON)
        return;
    m_mouse = g_pInputManager->getMouseCoordsInternal() - MON->m_position;
    if (m_closing)
        return;

    if (m_pressed && m_drag == DRAG_NONE && (m_mouse - m_pressAt).size() > THRESHOLD && m_pressTile >= 0) {
        const auto W = m_pressWindow.lock();
        if (W) {
            m_drag       = DRAG_WINDOW;
            const auto TS = tileScale();
            const auto WTOP = m_tiles[m_pressTile].pos + (W->m_realPosition->value() - MON->m_position) * TS;
            m_grabOffset = (m_pressAt - WTOP) * 1.04;
        } else {
            m_drag       = DRAG_WORKSPACE;
            m_grabOffset = m_pressAt - m_tiles[m_pressTile].pos;
        }
        Cursor::overrideController->setOverride("grabbing", Cursor::CURSOR_OVERRIDE_UNKNOWN);
    }

    if (m_drag != DRAG_NONE)
        m_hoverSlot = slotAt(m_mouse);
    else {
        const auto T = tileAt(m_mouse);
        if (T >= 0)
            m_selected = slotOf(m_order, T);
    }
}

void CExpo::onButton(uint32_t button, bool pressed) {
    if (m_closing || m_zoom->value() < 0.95)
        return;
    constexpr uint32_t LEFT = 0x110, RIGHT = 0x111; // BTN_LEFT, BTN_RIGHT

    if (pressed) {
        if (m_pressed || (button != LEFT && button != RIGHT))
            return;
        m_pressed     = true;
        m_pressButton = button;
        m_pressAt     = m_mouse;
        m_pressTile   = tileAt(m_mouse);
        m_pressWindow.reset();
        if (m_pressTile >= 0 && button == LEFT) {
            const bool ONBADGE = CBox{m_tiles[m_pressTile].pos + Vector2D{8, 8}, Vector2D{BADGE, BADGE}}.containsPoint(m_mouse);
            if (!ONBADGE)
                m_pressWindow = windowAt(m_pressTile, m_mouse);
        }
        return;
    }

    if (!m_pressed || button != m_pressButton)
        return;
    m_pressed = false;
    Cursor::overrideController->setOverride("left_ptr", Cursor::CURSOR_OVERRIDE_UNKNOWN);

    if (m_drag != DRAG_NONE && m_hoverSlot >= 0)
        m_selected = m_hoverSlot;
    if (m_drag == DRAG_WINDOW)
        dropWindow();
    else if (m_drag == DRAG_WORKSPACE)
        commitReorder();
    else if (m_pressTile < 0)
        close(-1); // a click off the grid goes back
    else {
        const auto W    = m_pressWindow.lock();
        const int  SLOT = slotOf(m_order, m_pressTile);
        close(SLOT);
        if (W && button == LEFT)
            Desktop::focusState()->fullWindowFocus(W);
    }
    m_drag      = DRAG_NONE;
    m_hoverSlot = -1;
    m_pressTile = -1;
    m_pressWindow.reset();
}

bool CExpo::onKey(uint32_t sym, bool pressed) {
    const bool OURS = sym == XKB_KEY_Escape || sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_space || sym == XKB_KEY_Left ||
        sym == XKB_KEY_Right || sym == XKB_KEY_Up || sym == XKB_KEY_Down || (sym >= XKB_KEY_1 && sym <= XKB_KEY_9);
    if (!OURS)
        return false;
    if (!pressed || m_closing)
        return true;
    switch (sym) {
        case XKB_KEY_Escape: close(-1); break;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
        case XKB_KEY_space: close(m_selected); break;
        case XKB_KEY_Left: m_selected = (m_selected + SLOTS - 1) % SLOTS; break;
        case XKB_KEY_Right: m_selected = (m_selected + 1) % SLOTS; break;
        case XKB_KEY_Up: m_selected = (m_selected + SLOTS - COLUMNS) % SLOTS; break;
        case XKB_KEY_Down: m_selected = (m_selected + COLUMNS) % SLOTS; break;
        default: close(sym - XKB_KEY_1); break;
    }
    return true;
}

// ---------------------------------------------------------------- what a drop does

void CExpo::dropWindow() {
    const auto W   = m_pressWindow.lock();
    const auto MON = m_monitor.lock();
    if (!W || !MON || m_hoverSlot < 0 || m_hoverSlot == slotOf(m_order, m_pressTile))
        return;
    const int  TILE = m_order[m_hoverSlot];
    const auto ID   = m_hoverSlot + 1; // tile in slot s is workspace s+1
    auto       WS   = m_tiles[TILE].ws.lock();
    if (!WS) {
        WS = g_pCompositor->getWorkspaceByID(ID);
        if (WS && WS->m_monitor != MON)
            return; // a workspace of that number on another monitor
        if (!WS)
            WS = g_pCompositor->createNewWorkspace(ID, MON->m_id);
        if (!WS)
            return;
        m_tiles[TILE].ws = WS;
    }
    g_pCompositor->moveWindowToWorkspaceSafe(W, WS);
}

void CExpo::commitReorder() {
    const auto ORDER = provisionalOrder();
    std::unordered_map<WORKSPACEID, WORKSPACEID> remap;
    for (int s = 0; s < SLOTS; ++s) {
        const int  t     = ORDER[s];
        const auto WS    = m_tiles[t].ws.lock();
        const auto OLDID = slotOf(m_order, t) + 1;
        if (WS && OLDID != s + 1) {
            if (WS->m_id != OLDID)
                return; // out of step with Hyprland: leave everything as it was
            remap[OLDID] = s + 1;
        }
    }
    // a workspace of a target number that isn't ours (another monitor's): don't collide with it
    for (const auto& [from, to] : remap) {
        const auto OTHER = g_pCompositor->getWorkspaceByID(to);
        if (OTHER && !remap.contains(to))
            return;
    }
    m_order = ORDER;
    if (!remap.empty())
        renumberWorkspaces(remap);
}

void CExpo::close(int slot) {
    if (m_closing)
        return;
    const auto MON = m_monitor.lock();
    if (!MON)
        return;
    m_closing = true;
    m_drag    = DRAG_NONE;
    m_pressed = false;

    if (slot < 0) {
        for (int s = 0; s < SLOTS; ++s)
            if (m_tiles[m_order[s]].ws && m_tiles[m_order[s]].ws.lock() == MON->m_activeWorkspace)
                slot = s;
    } else {
        const auto WS = m_tiles[m_order[slot]].ws.lock();
        if (WS != MON->m_activeWorkspace) {
            const auto OLD = MON->m_activeWorkspace;
            MON->setSpecialWorkspace(0);
            g_pKeybindManager->changeworkspace(std::to_string(slot + 1));
            if (MON->m_activeWorkspace)
                g_pDesktopAnimationManager->startAnimation(MON->m_activeWorkspace, CDesktopAnimationManager::ANIMATION_TYPE_IN, true, true);
            if (OLD)
                g_pDesktopAnimationManager->startAnimation(OLD, CDesktopAnimationManager::ANIMATION_TYPE_OUT, false, true);
            m_startedOn = MON->m_activeWorkspace;
            if (!m_tiles[m_order[slot]].ws)
                m_tiles[m_order[slot]].ws = MON->m_activeWorkspace;
        }
    }
    m_zoomSlot = slot;
    if (slot >= 0)
        m_tiles[m_order[slot]].pos = slotPos(slot); // land exactly
    *m_zoom = 0.F;
    m_zoom->setCallbackOnEnd([](auto) {
        g_pEventLoopManager->doLater([] {
            g_pExpo.reset();
            g_pInputManager->simulateMouseMovement(); // the pointer enters what's under it again
        });
    });
}
