-- res/sysgui/notepad.lua
local M = {}

_G.notepad_text = "This is a simple text document.\nYou can write text here using your keyboard."

local last_blink_state = -1

M.draw = function(win, mx, my, mdown, dt)
    drawRect(win.x, win.y, win.w, 24, 0x2E303B)
    drawText("NOTES.TXT", win.x + 8, win.y + 6, 0xE5E9F0)

    if button("SAVE", win.x + win.w - 55, win.y + 3, 50, 18) then
        saveFile("NOTES.TXT", _G.notepad_text)
        if type(_G.refresh_explorer) == "function" then
            _G.refresh_explorer()
        end
    end

    local text_y = win.y + 32
    local lines = {}
    for line in string.gmatch(_G.notepad_text, "[^\n]+") do
        table.insert(lines, line)
    end
    if _G.notepad_text == "" or string.sub(_G.notepad_text, -1) == "\n" then
        table.insert(lines, "")
    end

    for idx, line in ipairs(lines) do
        drawText(line, win.x + 8, text_y + (idx - 1) * 14, 0xE5E9F0)
    end

    local np_blink = math.floor(getUptime() * 2) % 2
    if np_blink ~= last_blink_state then
        _G.needs_redraw = true
        last_blink_state = np_blink
    end
    if np_blink == 0 then
        local last_line = lines[#lines] or ""
        local cur_x = win.x + 8 + string.len(last_line) * 8
        local cur_y = text_y + (#lines - 1) * 14
        drawRect(cur_x, cur_y, 2, 12, 0x8BE9FD)
    end
end

M.handle_key = function(win, key, char)
    if key == 28 then -- ENTER
        _G.notepad_text = _G.notepad_text .. "\n"
    elseif key == 14 then -- BACKSPACE
        _G.notepad_text = string.sub(_G.notepad_text, 1, -2)
    elseif string.len(char) > 0 and string.byte(char) >= 32 then
        _G.notepad_text = _G.notepad_text .. char
    end
end

return M