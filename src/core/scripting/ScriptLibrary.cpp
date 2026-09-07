// SPDX-License-Identifier: GPL-3.0-or-later
//
// TorqueBus Studio
// Copyright (C) TorqueBus contributors

#include "core/scripting/ScriptLibrary.h"

#include <utility>

namespace torquebus {

void ScriptLibrary::offer(const std::string& nodeId, std::string source)
{
    const std::lock_guard lock{m_mutex};
    m_pending[nodeId] = std::move(source);
}

std::vector<ScriptReload> ScriptLibrary::takeReports()
{
    std::vector<ScriptReload> taken;

    const std::lock_guard lock{m_mutex};
    taken.swap(m_reports);

    return taken;
}

bool ScriptLibrary::isPending(const std::string& nodeId) const
{
    const std::lock_guard lock{m_mutex};
    return m_pending.contains(nodeId);
}

bool ScriptLibrary::take(const std::string& nodeId, std::string& source)
{
    // try_lock, never lock: the other side of this mutex is a text editor in a
    // window, and the frame path must not wait for one. An offer the editor
    // happened to be holding is taken a pass later.
    const std::unique_lock lock{m_mutex, std::try_to_lock};

    if (!lock.owns_lock()) {
        return false;
    }

    const auto found = m_pending.find(nodeId);
    if (found == m_pending.end()) {
        return false;
    }

    source = std::move(found->second);
    m_pending.erase(found);

    return true;
}

void ScriptLibrary::report(ScriptReload outcome)
{
    // This one waits, and can afford to: a reload happens when somebody presses
    // a button, and losing the error message because the editor was reading
    // would leave them staring at a script that did not load for no stated
    // reason.
    const std::lock_guard lock{m_mutex};
    m_reports.push_back(std::move(outcome));
}

void ScriptLibrary::clear()
{
    const std::lock_guard lock{m_mutex};

    m_pending.clear();
    m_reports.clear();
}

} // namespace torquebus
