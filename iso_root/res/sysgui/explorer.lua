-- res/sysgui/explorer.lua
local files_list = {}

local function refresh_explorer()
    files_list = getFiles()
end
_G.refresh_explorer = refresh_explorer -- Делаем функцию глобальной для вызова из Notepad

local function draw_explorer(win, mx, my, mdown, dt)
    drawRect(win.x, win.y, win.w, 24, 0x2E303B)
    drawText("Root VFS Volume Directory", win.x + 8, win.y + 6, 0xD8DEE9)

    if button("REFR", win.x + win.w - 55, win.y + 3, 50, 18) then
        refresh_explorer()
    end

    local draw_y = win.y + 32
    if #files_list == 0 then
        drawText("No volumes or files found.", win.x + 20, draw_y, 0x888C94)
    end

    local row_h = 22
    local max_rows = math.max(0, math.floor((win.h - 32) / row_h))
    local dev_col_x = win.x + win.w - 120
    local name_col_x = win.x + 26
    local max_name_chars = math.max(0, math.floor((dev_col_x - name_col_x - 4) / 8))

    for idx = 1, math.min(#files_list, max_rows) do
        local f = files_list[idx]
        local row_y = draw_y + (idx - 1) * row_h
        local is_hover = (mx >= win.x and mx < win.x + win.w and my >= row_y and my < row_y + 20)

        if is_hover then
            drawRect(win.x, row_y, win.w, 20, 0x333644)
        end

        local icon_col = (f.dev == "EXT2_DISK") and 0x5E81AC or 0xEBCB8B
        drawRect(win.x + 8, row_y + 5, 10, 10, icon_col)

        -- Клиппинг длинных названий файлов
        local display_name = f.name or ""
        if #display_name > max_name_chars and max_name_chars > 1 then
            display_name = string.sub(display_name, 1, max_name_chars - 1) .. "~"
        end
        drawText(display_name, name_col_x, row_y + 3, 0xE5E9F0)
        drawText(f.dev, dev_col_x, row_y + 3, 0x888C94)

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
                local content = readFile(f.name)
                if content then
                    _G.notepad_text = content
                else
                    _G.notepad_text = "Empty file"
                end
                focused_window = notepad_window
            end
        end
    end
end

return draw_explorer