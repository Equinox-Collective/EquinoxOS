--- START OF FILE res/sysgui/explorer.lua ---
local files_list = {}

local function refresh_explorer()
    files_list = getFiles() or {}
end
_G.refresh_explorer = refresh_explorer

local function draw_explorer(win, mx, my, mdown, dt)
    drawRect(win.x, win.y, win.w, 30, 0x21252B)
    drawText("Virtual File System Explorer", win.x + 10, win.y + 9, 0x61AFEF)

    if button("REFR", win.x + win.w - 55, win.y + 6, 45, 18) then
        refresh_explorer()
    end

    local draw_y = win.y + 36
    if #files_list == 0 then
        drawText("VFS device registry empty.", win.x + 20, draw_y, 0x5C6370)
    end

    local row_h = 22
    local max_rows = math.max(0, math.floor((win.h - 40) / row_h))
    local dev_col_x = win.x + win.w - 120
    local name_col_x = win.x + 30
    local max_name_chars = math.max(0, math.floor((dev_col_x - name_col_x - 10) / 8))

    for idx = 1, math.min(#files_list, max_rows) do
        local f = files_list[idx]
        local row_y = draw_y + (idx - 1) * row_h
        local is_hover = (mx >= win.x and mx < win.x + win.w and my >= row_y and my < row_y + 20)

        if is_hover then
            drawRect(win.x, row_y, win.w, 20, 0x2C313C)
        end

        local icon_col = (f.dev == "EXT2_DISK") and 0x61AFEF or 0xE5C07B
        drawRect(win.x + 10, row_y + 5, 10, 10, icon_col)

        local display_name = f.name or ""
        if #display_name > max_name_chars and max_name_chars > 1 then
            display_name = string.sub(display_name, 1, max_name_chars - 1) .. "~"
        end
        drawText(display_name, name_col_x, row_y + 3, 0xABB2BF)
        drawText(f.dev, dev_col_x, row_y + 3, 0x5C6370)

        if is_hover and mdown and not last_mdown then
            local notepad_window = nil
            for _, w in ipairs(windows) do
                if w.title == "Notepad Text Editor" then
                    notepad_window = w
                    break
                end
            end
            
            if notepad_window then
                notepad_window.active = true
                notepad_window.minimized = false
                local content = readFile(f.name)
                _G.notepad_text = content or "Empty storage buffer."
                focused_window = notepad_window
                bring_to_front(notepad_window)
            end
        end
    end
end

return draw_explorer
--- END OF FILE res/sysgui/explorer.lua ---