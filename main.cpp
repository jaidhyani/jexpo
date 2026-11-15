// jexpo's entry points: the hooks that let it draw in place of a monitor's workspace (taken from
// hyprexpo), the jexpo:expo dispatcher, and jexpo:renumber for doing a reorder from a script.

#include "expo.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/string/VarList.hpp>

inline CFunctionHook* g_pRenderWorkspaceHook = nullptr;
inline CFunctionHook* g_pAddDamageHookA      = nullptr;
inline CFunctionHook* g_pAddDamageHookB      = nullptr;
typedef void (*origRenderWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, timespec*, const CBox&);
typedef void (*origAddDamageA)(void*, const CBox&);
typedef void (*origAddDamageB)(void*, const pixman_region32_t*);

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static void hkRenderWorkspace(void* thisptr, PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace, timespec* now, const CBox& geometry) {
    if (!g_pExpo || g_pExpo->m_blockRender || g_pExpo->m_monitor != pMonitor)
        ((origRenderWorkspace)(g_pRenderWorkspaceHook->m_original))(thisptr, pMonitor, pWorkspace, now, geometry);
    else
        g_pExpo->render();
}

static void hkAddDamageA(void* thisptr, const CBox& box) {
    const auto PMONITOR = (CMonitor*)thisptr;
    if (!g_pExpo || g_pExpo->m_monitor != PMONITOR->m_self || g_pExpo->m_blockDamage) {
        ((origAddDamageA)g_pAddDamageHookA->m_original)(thisptr, box);
        return;
    }
    g_pExpo->onDamage();
}

static void hkAddDamageB(void* thisptr, const pixman_region32_t* rg) {
    const auto PMONITOR = (CMonitor*)thisptr;
    if (!g_pExpo || g_pExpo->m_monitor != PMONITOR->m_self || g_pExpo->m_blockDamage) {
        ((origAddDamageB)g_pAddDamageHookB->m_original)(thisptr, rg);
        return;
    }
    g_pExpo->onDamage();
}

static SDispatchResult onExpo(std::string arg) {
    if (arg == "toggle" || arg.empty()) {
        if (g_pExpo)
            g_pExpo->close();
        else if (const auto MON = Desktop::focusState()->monitor(); MON && MON->m_activeWorkspace)
            g_pExpo = std::make_unique<CExpo>(MON);
        return {};
    }
    if (arg == "close" || arg == "off") {
        if (g_pExpo)
            g_pExpo->close();
        return {};
    }
    return {.success = false, .error = "jexpo:expo takes toggle or close"};
}

// "jexpo:renumber 7 2": workspace 7 taken out and put back at 2, 2-6 shifting to 3-7, as a
// workspace drag in the expo does.
static SDispatchResult onRenumber(std::string arg) {
    Hyprutils::String::CVarList args(arg, 0, ' ');
    int                         from = 0, to = 0;
    try {
        from = std::stoi(args[0]);
        to   = std::stoi(args[1]);
    } catch (...) { return {.success = false, .error = "usage: jexpo:renumber <from> <to>, both 1-9"}; }
    if (from < 1 || from > SLOTS || to < 1 || to > SLOTS)
        return {.success = false, .error = "usage: jexpo:renumber <from> <to>, both 1-9"};
    if (g_pExpo)
        return {.success = false, .error = "close the expo first"};
    std::vector<int> order;
    for (int i = 1; i <= SLOTS; ++i)
        order.push_back(i);
    order.erase(order.begin() + from - 1);
    order.insert(order.begin() + to - 1, from);
    std::unordered_map<WORKSPACEID, WORKSPACEID> remap;
    for (int s = 0; s < SLOTS; ++s)
        if (order[s] != s + 1 && g_pCompositor->getWorkspaceByID(order[s]))
            remap[order[s]] = s + 1;
    renumberWorkspaces(remap);
    return {};
}

static void fail(const std::string& reason) {
    HyprlandAPI::addNotification(PHANDLE, "[jexpo] " + reason, CHyprColor{1.0, 0.2, 0.2, 1.0}, 8000);
    throw std::runtime_error("[jexpo] " + reason);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    if (std::string{__hyprland_api_get_hash()} != std::string{__hyprland_api_get_client_hash()})
        fail("built for a different Hyprland version: rebuild it (make, or ./build)");

    auto hook = [](const std::string& name, void* fn) {
        auto fns = HyprlandAPI::findFunctionsByName(PHANDLE, name);
        if (fns.empty())
            fail("no function " + name + " to hook");
        return HyprlandAPI::createFunctionHook(PHANDLE, fns[0].address, fn);
    };
    g_pRenderWorkspaceHook = hook("renderWorkspace", (void*)hkRenderWorkspace);
    g_pAddDamageHookB      = hook("addDamageEPK15pixman_region32", (void*)hkAddDamageB);
    g_pAddDamageHookA      = hook("_ZN8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE", (void*)hkAddDamageA);
    if (!g_pRenderWorkspaceHook->hook() || !g_pAddDamageHookA->hook() || !g_pAddDamageHookB->hook())
        fail("couldn't hook the renderer");

    static auto preRender = HyprlandAPI::registerCallbackDynamic(PHANDLE, "preRender", [](void*, SCallbackInfo&, std::any param) {
        if (g_pExpo && std::any_cast<PHLMONITOR>(param) == g_pExpo->m_monitor.lock())
            g_pExpo->onPreRender();
    });

    HyprlandAPI::addConfigValue(PHANDLE, "plugin:jexpo:tile_hz", Hyprlang::INT{30});  // a tile with windows redraws this often
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:jexpo:empty_hz", Hyprlang::INT{2});  // an empty one (wallpaper, bar)
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:jexpo:col_bg", Hyprlang::INT{0xFF161A17});     // behind the grid
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:jexpo:col_accent", Hyprlang::INT{0xFFC8D8A4}); // outlines, current number
    HyprlandAPI::addConfigValue(PHANDLE, "plugin:jexpo:font", Hyprlang::STRING{"monospace"});   // the numbers and the hint
    HyprlandAPI::addDispatcherV2(PHANDLE, "jexpo:expo", onExpo);
    HyprlandAPI::addDispatcherV2(PHANDLE, "jexpo:renumber", onRenumber);

    return {"jexpo", "Workspaces 1-9, live, to drag windows between and reorder", "jaidhyani", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pExpo.reset();
    g_pHyprRenderer->makeEGLCurrent();
    g_text = {};
    g_pHyprRenderer->m_renderPass.removeAllOfType("CExpoPassElement");
}
