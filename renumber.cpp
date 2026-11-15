// Renumbering workspaces in place. Hyprland has no dispatcher for it: a workspace's number is
// its m_id, fixed at creation, and "workspace 3" means whichever workspace has id 3. Windows,
// monitors and focus hold workspaces by pointer, so changing m_id renumbers a workspace without
// touching what's in it. What holds the number itself, and is rewritten here:
//
// - each workspace's m_id, and its name when the name is just the number
// - the layouts' per-window and per-workspace records (dwindle's nodes, master's nodes and
//   orientation data), which find a workspace's windows by id
// - windows' m_lastWorkspace, monitors' previous-workspace stacks, the monitor->workspace memory
//
// Then IPC listeners (waybar) are told: each renumbered workspace destroyed under its old number
// and created under its new one, its windows moved, and the active workspace again.
// The ext-workspace protocol sends a workspace's id only when it's created, so a client of that
// sees the new names but keeps the old ids until it reconnects.

#include "expo.hpp"

#include <stack>

#define private public
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/helpers/Monitor.hpp>
#include <hyprland/src/layout/DwindleLayout.hpp>
#include <hyprland/src/layout/MasterLayout.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/LayoutManager.hpp>
#undef private

void renumberWorkspaces(const std::unordered_map<WORKSPACEID, WORKSPACEID>& remap) {
    auto to = [&](WORKSPACEID id) {
        const auto IT = remap.find(id);
        return IT == remap.end() ? id : IT->second;
    };

    std::vector<PHLWORKSPACE> changed;
    for (const auto& ref : g_pCompositor->getWorkspaces()) {
        const auto WS = ref.lock();
        if (WS && !WS->inert() && !WS->m_isSpecialWorkspace && remap.contains(WS->m_id) && to(WS->m_id) != WS->m_id)
            changed.push_back(WS);
    }
    if (changed.empty())
        return;

    for (const auto& ws : changed) {
        g_pEventManager->postEvent({.event = "destroyworkspace", .data = ws->m_name});
        g_pEventManager->postEvent({.event = "destroyworkspacev2", .data = std::format("{},{}", ws->m_id, ws->m_name)});
    }

    for (const auto& ws : changed) {
        const auto OLD = ws->m_id;
        ws->m_id       = to(OLD);
        if (ws->m_name == std::to_string(OLD))
            ws->m_name = std::to_string(ws->m_id);
    }

    for (const auto& [name, layout] : g_pLayoutManager->m_layouts) {
        if (name == "dwindle") {
            for (auto& node : static_cast<CHyprDwindleLayout*>(layout)->m_dwindleNodesData)
                node->workspaceID = to(node->workspaceID);
        } else if (name == "master") {
            auto* master = static_cast<CHyprMasterLayout*>(layout);
            for (auto& node : master->m_masterNodesData)
                node.workspaceID = to(node.workspaceID);
            for (auto& data : master->m_masterWorkspacesData)
                data.workspaceID = to(data.workspaceID);
        }
    }

    for (const auto& w : g_pCompositor->m_windows)
        if (!w->m_workspace)
            w->m_lastWorkspace = to(w->m_lastWorkspace);

    for (const auto& mon : g_pCompositor->m_monitors) {
        std::vector<WORKSPACEID> prev;
        while (!mon->m_prevWorkSpaces.empty()) {
            prev.push_back(to(mon->m_prevWorkSpaces.top()));
            mon->m_prevWorkSpaces.pop();
        }
        for (auto it = prev.rbegin(); it != prev.rend(); ++it)
            mon->m_prevWorkSpaces.push(*it);
    }
    for (auto& [monitor, id] : g_pCompositor->m_seenMonitorWorkspaceMap)
        id = to(id);

    for (const auto& ws : changed) {
        g_pEventManager->postEvent({.event = "createworkspace", .data = ws->m_name});
        g_pEventManager->postEvent({.event = "createworkspacev2", .data = std::format("{},{}", ws->m_id, ws->m_name)});
        ws->m_events.renamed.emit();
        ws->updateWindowData();
        ws->updateWindows();
    }
    for (const auto& w : g_pCompositor->m_windows) {
        if (!w->m_isMapped || !w->m_workspace || std::ranges::find(changed, w->m_workspace) == changed.end())
            continue;
        g_pEventManager->postEvent({.event = "movewindow", .data = std::format("{:x},{}", (uintptr_t)w.get(), w->m_workspace->m_name)});
        g_pEventManager->postEvent({.event = "movewindowv2", .data = std::format("{:x},{},{}", (uintptr_t)w.get(), w->m_workspace->m_id, w->m_workspace->m_name)});
    }
    for (const auto& mon : g_pCompositor->m_monitors) {
        const auto ACTIVE = mon->m_activeWorkspace;
        if (!ACTIVE || std::ranges::find(changed, ACTIVE) == changed.end())
            continue;
        g_pEventManager->postEvent({.event = "workspace", .data = ACTIVE->m_name});
        g_pEventManager->postEvent({.event = "workspacev2", .data = std::format("{},{}", ACTIVE->m_id, ACTIVE->m_name)});
    }
}
