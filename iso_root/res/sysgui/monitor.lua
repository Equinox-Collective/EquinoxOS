-- res/sysgui/monitor.lua
local function draw_monitor(win, mx, my, mdown, dt)
    local used, total = getMemInfo()
    local used_mb = math.floor(used / (1024 * 1024))
    local total_mb = math.floor(total / (1024 * 1024))

    local ram_str = string.format("System RAM: %d / %d MB", used_mb, total_mb)
    drawText(ram_str, win.x + 15, win.y + 20, 0xE5E9F0)

    -- Полоса RAM
    drawRect(win.x + 15, win.y + 40, win.w - 30, 12, 0x2E3440)
    local ratio = used / total
    local fill_w = math.floor(ratio * (win.w - 30))
    drawRect(win.x + 15, win.y + 40, fill_w, 12, 0x0078D7)

    -- Аптайм
    local uptime = getUptime()
    local s = math.floor(uptime % 60)
    local m = math.floor((uptime / 60) % 60)
    local h = math.floor(uptime / 3600)
    local uptime_str = string.format("Uptime: %02d:%02d:%02d", h, m, s)
    drawText(uptime_str, win.x + 15, win.y + 75, 0x888C94)
end

return draw_monitor