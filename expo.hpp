#pragma once
// jexpo: workspaces 1-9 as a live 3x3 grid (Super+Tab). Windows drag between workspaces;
// workspaces drag to new places, the others making room as you go, and dropping one renumbers
// the workspaces themselves (renumber.cpp), so nothing inside them moves.

#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/managers/HookSystemManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <vector>

inline HANDLE PHANDLE = nullptr;

constexpr int SLOTS   = 9;
constexpr int COLUMNS = 3;

// Workspaces taken out of their order and put back at a new place: workspace id renumbering
// applied to Hyprland's own state (renumber.cpp). Keys and values are ids 1-9.
void renumberWorkspaces(const std::unordered_map<WORKSPACEID, WORKSPACEID>& remap);

class CExpo {
  public:
    explicit CExpo(PHLMONITOR monitor);
    ~CExpo();

    void          render();     // called in place of the monitor's workspace
    void          fullRender(); // the pass element's draw
    void          onPreRender();
    void          onDamage();
    void          close(int slot = -1); // zoom into slot (go there), or back to where you were

    PHLMONITORREF m_monitor;
    bool          m_blockRender = false; // while drawing a tile, the hook renders for real
    bool          m_blockDamage = false;
    bool          m_closing     = false;

  private:
    struct STile {
        PHLWORKSPACEREF ws;   // null: an empty workspace
        CFramebuffer    fb;
        Vector2D        pos;  // where it's drawn now, logical, top left; moves toward its slot
        std::chrono::steady_clock::time_point drawnAt;
        bool            drawn = false;
    };

    // how it's keeping up, written to $XDG_RUNTIME_DIR/jexpo-stats when it closes
    struct {
        int    frames = 0;
        double cpuMs = 0, cpuMaxMs = 0, intervalMs = 0, intervalMaxMs = 0;
    } m_stats;

    // geometry, logical
    Vector2D slotPos(int slot) const;
    Vector2D tileSize() const;
    double   tileScale() const;
    int      slotAt(const Vector2D& p) const;   // grid slot under a point, or -1
    int      tileAt(const Vector2D& p) const;   // tile drawn under a point, or -1
    CBox     zoomedBox(const Vector2D& pos) const;
    PHLWINDOW windowAt(int tile, const Vector2D& p) const;
    std::vector<int> provisionalOrder() const;  // slot -> tile, with the dragged one moved

    void redrawTile(int tile);
    void redrawDragged();
    void commitReorder();
    void dropWindow();
    void onMouseMove();
    void onButton(uint32_t button, bool pressed);
    bool onKey(uint32_t keysym, bool pressed); // true: handled (cancel it)

    SP<CTexture> textTexture(const std::string& text, int px, const CHyprColor& color, Vector2D& sizeOut);

    std::vector<STile>    m_tiles;  // m_tiles[i] is the workspace whose number was i+1 at open
    std::vector<int>      m_order;  // m_order[slot] = tile shown in that slot
    PHLWORKSPACEREF       m_startedOn;
    int                   m_zoomSlot = 0;  // the slot the zoom starts from / ends in
    PHLANIMVAR<float>     m_zoom;          // 0: m_zoomSlot fills the screen; 1: the grid
    std::chrono::steady_clock::time_point m_lastFrame;

    // the mouse
    Vector2D              m_mouse;        // logical, monitor-local
    Vector2D              m_pressAt;
    bool                  m_pressed = false;
    uint32_t              m_pressButton = 0;
    int                   m_pressTile = -1;
    PHLWINDOWREF          m_pressWindow;
    enum eDrag { DRAG_NONE, DRAG_WINDOW, DRAG_WORKSPACE } m_drag = DRAG_NONE;
    Vector2D              m_grabOffset;   // cursor minus the dragged thing's top left, logical
    int                   m_hoverSlot = -1;
    int                   m_selected  = 0; // keyboard choice / mouse hover, a slot
    CFramebuffer          m_dragFb;       // the dragged window, drawn alone

    SP<HOOK_CALLBACK_FN> m_mouseMoveHook, m_mouseButtonHook, m_keyHook;
    SP<CEventLoopTimer>  m_frameTimer; // asks for every frame while open (see the constructor)

    friend class CExpoPassElement;
};

inline std::unique_ptr<CExpo> g_pExpo;

// The badges' numbers and the hint line, kept across opens; released when the plugin unloads.
struct STextCache {
    double                    scale = 0;
    std::string               font;
    uint32_t                  accent = 0;
    std::vector<SP<CTexture>> number, numberCurrent;
    SP<CTexture>              hint;
    Vector2D                  numberSize, hintSize;
};
inline STextCache g_text;
