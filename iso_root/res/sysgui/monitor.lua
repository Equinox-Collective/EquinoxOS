--- START OF FILE res/sysgui/monitor.lua ---
local M = {}

local ram_history = {}
local task_history = {}
local max_points = 32
local last_update_time = 0
local selected_task_idx = 1

M.draw = function(win, mx, my, mdown, dt)
    local used, total = getMemInfo()
    local used_mb = math.floor(used / (1024 * 1024))
    local total_mb = math.floor(total / (1024 * 1024))
    local ram_ratio = total > 0 and (used / total) or 0

    local tasks = getTasks() or {}
    local task_count = #tasks

    -- Resource Tracker Step Timer
    local now = getUptime()
    if now - last_update_time >= 0.5 then
        table.insert(ram_history, ram_ratio)
        if #ram_history > max_points then table.remove(ram_history, 1) end
        
        -- Cap task representation scale at 16 active processes
        local task_ratio = math.min(task_count / 16, 1.0)
        table.insert(task_history, task_ratio)
        if #task_history > max_points then table.remove(task_history, 1) end
        
        last_update_time = now
    end

    -- Split-pane Layout
    local graph_x = win.x + 10
    local graph_y = win.y + 12
    local graph_w = math.floor(win.w / 2) - 15
    local graph_h = 75

    -- Render RAM Graph Pane
    drawRect(graph_x, graph_y, graph_w, graph_h, 0x1E2227)
    drawText(string.format("RAM: %dMB", used_mb), graph_x + 5, graph_y + 5, 0x61AFEF)
    if #ram_history > 1 then
        local step = graph_w / (max_points - 1)
        for i = 1, #ram_history - 1 do
            local x1 = graph_x + math.floor((i - 1) * step)
            local y1 = graph_y + graph_h - math.floor(ram_history[i] * (graph_h - 15)) - 3
            local x2 = graph_x + math.floor(i * step)
            local y2 = graph_y + graph_h - math.floor(ram_history[i+1] * (graph_h - 15)) - 3
            drawLine(x1, y1, x2, y2, 0x61AFEF)
        end
    end

    -- Render Tasks Graph Pane
    local gt_x = win.x + math.floor(win.w / 2) + 5
    drawRect(gt_x, graph_y, graph_w, graph_h, 0x1E2227)
    drawText(string.format("Tasks: %d", task_count), gt_x + 5, graph_y + 5, 0x98C379)
    if #task_history > 1 then
        local step = graph_w / (max_points - 1)
        for i = 1, #task_history - 1 do
            local x1 = gt_x + math.floor((i - 1) * step)
            local y1 = graph_y + graph_h - math.floor(task_history[i] * (graph_h - 15)) - 3
            local x2 = gt_x + math.floor(i * step)
            local y2 = graph_y + graph_h - math.floor(task_history[i+1] * (graph_h - 15)) - 3
            drawLine(x1, y1, x2, y2, 0x98C379)
        end
    end

    -- Lower Pane: Interactive Process Killer
    local list_y = win.y + 95
    local list_h = win.h - 105
    drawRect(win.x + 10, list_y, win.w - 20, list_h, 0x1E2227)
    drawRect(win.x + 10, list_y, win.w - 20, 16, 0x282C34)
    drawText("PID   CR3 ADDRESS        STATUS", win.x + 15, list_y + 2, 0xABB2BF)

    local item_h = 16
    local max_items = math.floor((list_h - 16) / item_h)
    
    for idx = 1, math.min(task_count, max_items) do
        local t = tasks[idx]
        local row_y = list_y + 16 + (idx - 1) * item_h
        local row_hover = mx >= win.x + 10 and mx < win.x + win.w - 10 and my >= row_y and my < row_y + item_h
        
        if idx == selected_task_idx then
            drawRect(win.x + 12, row_y, win.w - 24, item_h, 0x2C313C)
        elseif row_hover then
            drawRect(win.x + 12, row_y, win.w - 24, item_h, 0x21252B)
        end

        local info_str = string.format("%-4d  0x%-14X  %s", t.pid, t.cr3, t.state)
        drawText(info_str, win.x + 15, row_y + 2, idx == selected_task_idx and 0x61AFEF or 0xABB2BF)

        if row_hover and mdown and not last_mdown then
            selected_task_idx = idx
        end
    end

    -- Contextual Action Trigger Button
    local selected_task = tasks[selected_task_idx]
    if selected_task then
        local kill_btn_x = win.x + win.w - 90
        local kill_btn_y = win.y + 98
        if button("KILL TASK", kill_btn_x, kill_btn_y, 75, 12) then
            killTask(selected_task.pid)
            selected_task_idx = 1
        end
    end
end

return M
--- END OF FILE res/sysgui/monitor.lua ---