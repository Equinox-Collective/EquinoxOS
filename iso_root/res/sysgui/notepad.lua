--- START OF FILE res/sysgui/notepad.lua ---
local M = {}

_G.notepad_text = _G.notepad_text or "local function greet()\n    print(\"Hello Equinox!\") -- Dynamic highlighter\nend\n"

local lines = nil
local cur_line = 1
local cur_col = 1
local scroll_offset = 0
local mode = "NORMAL" -- NORMAL, INSERT, COMMAND
local command_text = ""
local pending_key = nil
local yanked_line = nil
local last_notepad_text = nil

local status_message = nil
local status_timer = 0
local undo_stack = {}

local function push_undo(state_lines)
    local snap = {}
    for i, l in ipairs(state_lines) do snap[i] = l end
    table.insert(undo_stack, snap)
    if #undo_stack > 50 then table.remove(undo_stack, 1) end
end

local function pop_undo()
    if #undo_stack > 0 then
        local snap = table.remove(undo_stack)
        local restored = {}
        for i, l in ipairs(snap) do restored[i] = l end
        return restored
    end
    return nil
end

local function sync_to_lines()
    local text = _G.notepad_text or ""
    local t_lines = {}
    for line in string.gmatch(text .. "\n", "(.-)\r?\n") do
        table.insert(t_lines, line)
    end
    if #t_lines == 0 then t_lines = {""} end
    return t_lines
end

local function sync_to_text(t_lines)
    _G.notepad_text = table.concat(t_lines, "\n")
end

local function do_save(filename)
    sync_to_text(lines)
    saveFile(filename, _G.notepad_text)
    if type(_G.refresh_explorer) == "function" then
        _G.refresh_explorer()
    end
    status_message = '"' .. filename .. '" saved'
    status_timer = getUptime() + 3
end

-- Syntax Highlighting Tokenizer for Code Editing
local function draw_highlighted_text(win, line_text, start_x, line_y)
    local x = start_x
    local i = 1
    local len = #line_text
    
    local keywords = {
        ["local"] = 0xC678DD, ["function"] = 0xC678DD, ["if"] = 0xC678DD, ["then"] = 0xC678DD,
        ["else"] = 0xC678DD, ["end"] = 0xC678DD, ["return"] = 0xC678DD, ["for"] = 0xC678DD,
        ["in"] = 0xC678DD, ["nil"] = 0xD19A66, ["true"] = 0xD19A66, ["false"] = 0xD19A66,
        ["while"] = 0xC678DD, ["do"] = 0xC678DD, ["break"] = 0xC678DD, ["int"] = 0x56B6C2,
        ["char"] = 0x56B6C2, ["void"] = 0x56B6C2, ["double"] = 0x56B6C2, ["float"] = 0x56B6C2,
        ["#include"] = 0xE06C75, ["#define"] = 0xE06C75, ["static"] = 0xC678DD, ["const"] = 0xC678DD
    }
    
    while i <= len do
        local char = string.sub(line_text, i, i)
        
        -- Comments Checker
        if char == "-" and string.sub(line_text, i+1, i+1) == "-" then
            drawText(string.sub(line_text, i), x, line_y, 0x5C6370)
            break
        elseif char == "/" and string.sub(line_text, i+1, i+1) == "/" then
            drawText(string.sub(line_text, i), x, line_y, 0x5C6370)
            break
        -- Strings Parser
        elseif char == '"' or char == "'" then
            local quote = char
            local start_idx = i
            i = i + 1
            while i <= len and string.sub(line_text, i, i) ~= quote do
                i = i + 1
            end
            local str_lit = string.sub(line_text, start_idx, i)
            drawText(str_lit, x, line_y, 0x98C379)
            x = x + #str_lit * 8
            i = i + 1
        -- Numbers Parser
        elseif char:match("%d") then
            local start_idx = i
            while i <= len and string.sub(line_text, i, i):match("[%d%.xX%x]") do
                i = i + 1
            end
            local num_lit = string.sub(line_text, start_idx, i - 1)
            drawText(num_lit, x, line_y, 0xD19A66)
            x = x + #num_lit * 8
        -- Word Keywords
        elseif char:match("[%a_]") then
            local start_idx = i
            while i <= len and string.sub(line_text, i, i):match("[%w_]") do
                i = i + 1
            end
            local word = string.sub(line_text, start_idx, i - 1)
            local color = keywords[word] or 0xABB2BF
            drawText(word, x, line_y, color)
            x = x + #word * 8
        else
            drawText(char, x, line_y, 0xABB2BF)
            x = x + 8
            i = i + 1
        end
    end
end

M.draw = function(win, mx, my, mdown, dt)
    if _G.notepad_text ~= last_notepad_text then
        lines = sync_to_lines()
        last_notepad_text = _G.notepad_text
        cur_line = 1
        cur_col = 1
        scroll_offset = 0
    end

    drawRect(win.x, win.y, win.w, 24, 0x21252B)
    drawText("VIM.LUA - NOTES.TXT", win.x + 8, win.y + 6, 0x61AFEF)

    if button("SAVE", win.x + win.w - 55, win.y + 3, 50, 18) then
        do_save("NOTES.TXT")
    end

    local line_h = 14
    local text_y = win.y + 28
    local status_bar_h = 18
    local max_visible_lines = math.floor((win.h - 24 - status_bar_h) / line_h)

    if cur_line > scroll_offset + max_visible_lines then
        scroll_offset = cur_line - max_visible_lines
    elseif cur_line < scroll_offset + 1 then
        scroll_offset = cur_line - 1
    end

    local end_visible = math.min(#lines, scroll_offset + max_visible_lines)
    for idx = scroll_offset + 1, end_visible do
        local line_y = text_y + (idx - scroll_offset - 1) * line_h
        local line_text = lines[idx] or ""

        if idx == cur_line then
            drawRect(win.x, line_y, win.w, line_h, 0x2C313C)
        end

        draw_highlighted_text(win, line_text, win.x + 8, line_y)
    end

    -- Draw Mode-dependent Vim Cursor
    if cur_line >= scroll_offset + 1 and cur_line <= end_visible then
        local cur_y_screen = text_y + (cur_line - scroll_offset - 1) * line_h
        local cur_x_screen = win.x + 8 + (cur_col - 1) * 8

        if mode == "NORMAL" then
            drawRect(cur_x_screen, cur_y_screen, 8, 14, 0x98C379)
            local current_line_text = lines[cur_line] or ""
            local char_at_cursor = string.sub(current_line_text, cur_col, cur_col)
            if char_at_cursor == "" then char_at_cursor = " " end
            drawText(char_at_cursor, cur_x_screen, cur_y_screen, 0x282C34)
        elseif mode == "INSERT" then
            drawRect(cur_x_screen, cur_y_screen, 2, 14, 0x61AFEF)
        end
    end

    local bottom_y = win.y + win.h - status_bar_h
    if mode == "COMMAND" then
        drawRect(win.x, bottom_y, win.w, status_bar_h, 0x1E222B)
        drawText(":" .. command_text, win.x + 8, bottom_y + 2, 0xABB2BF)
    elseif status_message and getUptime() < status_timer then
        drawRect(win.x, bottom_y, win.w, status_bar_h, 0x21252B)
        drawText(status_message, win.x + 8, bottom_y + 2, 0x98C379)
    else
        drawRect(win.x, bottom_y, win.w, status_bar_h, 0x21252B)
        local mode_color = mode == "INSERT" and 0x61AFEF or 0x98C379
        drawRect(win.x, bottom_y, 65, status_bar_h, mode_color)
        drawText(mode, win.x + 8, bottom_y + 2, 0x282C34)

        drawText("NOTES.TXT", win.x + 75, bottom_y + 2, 0xABB2BF)
        local pos_str = string.format("L:%d/%d C:%d", cur_line, #lines, cur_col)
        local pos_w = string.len(pos_str) * 8
        drawText(pos_str, win.x + win.w - pos_w - 10, bottom_y + 2, 0xABB2BF)
    end
end

M.handle_key = function(win, key, char)
    if not lines then lines = sync_to_lines() end

    if mode == "INSERT" then
        if key == 1 or key == KEY_ESC then
            push_undo(lines)
            mode = "NORMAL"
            if cur_col > 1 and cur_col > string.len(lines[cur_line]) then
                cur_col = math.max(1, string.len(lines[cur_line]))
            end
        elseif key == 14 or key == KEY_BACKSPACE then
            if cur_col > 1 then
                local line = lines[cur_line]
                lines[cur_line] = string.sub(line, 1, cur_col - 2) .. string.sub(line, cur_col)
                cur_col = cur_col - 1
            elseif cur_line > 1 then
                local prev_line = lines[cur_line - 1]
                local prev_len = string.len(prev_line)
                lines[cur_line - 1] = prev_line .. lines[cur_line]
                table.remove(lines, cur_line)
                cur_line = cur_line - 1
                cur_col = prev_len + 1
            end
        elseif key == 28 or key == KEY_ENTER then
            local line = lines[cur_line]
            local left = string.sub(line, 1, cur_col - 1)
            local right = string.sub(line, cur_col)
            lines[cur_line] = left
            table.insert(lines, cur_line + 1, right)
            cur_line = cur_line + 1
            cur_col = 1
        elseif key == KEY_UP or key == 0x148 then
            if cur_line > 1 then
                cur_line = cur_line - 1
                cur_col = math.min(cur_col, string.len(lines[cur_line]) + 1)
            end
        elseif key == KEY_DOWN or key == 0x150 then
            if cur_line < #lines then
                cur_line = cur_line + 1
                cur_col = math.min(cur_col, string.len(lines[cur_line]) + 1)
            end
        elseif key == KEY_LEFT or key == 0x14B then
            cur_col = math.max(1, cur_col - 1)
        elseif key == KEY_RIGHT or key == 0x14D then
            cur_col = math.min(string.len(lines[cur_line]) + 1, cur_col + 1)
        elseif string.len(char) > 0 and string.byte(char) >= 32 then
            local line = lines[cur_line] or ""
            lines[cur_line] = string.sub(line, 1, cur_col - 1) .. char .. string.sub(line, cur_col)
            cur_col = cur_col + string.len(char)
        end
        sync_to_text(lines)
        last_notepad_text = _G.notepad_text

    elseif mode == "NORMAL" then
        if key == KEY_UP or key == 0x148 or char == "k" then
            if cur_line > 1 then
                cur_line = cur_line - 1
                cur_col = math.min(cur_col, math.max(1, string.len(lines[cur_line])))
            end
        elseif key == KEY_DOWN or key == 0x150 or char == "j" then
            if cur_line < #lines then
                cur_line = cur_line + 1
                cur_col = math.min(cur_col, math.max(1, string.len(lines[cur_line])))
            end
        elseif key == KEY_LEFT or key == 0x14B or char == "h" then
            cur_col = math.max(1, cur_col - 1)
        elseif key == KEY_RIGHT or key == 0x14D or char == "l" then
            cur_col = math.min(math.max(1, string.len(lines[cur_line])), cur_col + 1)
        elseif char == "i" then
            push_undo(lines)
            mode = "INSERT"
        elseif char == "a" then
            push_undo(lines)
            mode = "INSERT"
            cur_col = math.min(string.len(lines[cur_line]) + 1, cur_col + 1)
        elseif char == "o" then
            push_undo(lines)
            table.insert(lines, cur_line + 1, "")
            cur_line = cur_line + 1
            cur_col = 1
            mode = "INSERT"
            sync_to_text(lines)
            last_notepad_text = _G.notepad_text
        elseif char == "x" then
            push_undo(lines)
            local line = lines[cur_line]
            if string.len(line) > 0 then
                lines[cur_line] = string.sub(line, 1, cur_col - 1) .. string.sub(line, cur_col + 1)
                cur_col = math.min(cur_col, math.max(1, string.len(lines[cur_line])))
                sync_to_text(lines)
                last_notepad_text = _G.notepad_text
            end
        elseif char == "d" then
            if pending_key == "d" then
                push_undo(lines)
                yanked_line = lines[cur_line]
                table.remove(lines, cur_line)
                if #lines == 0 then lines = {""} end
                if cur_line > #lines then cur_line = #lines end
                cur_col = math.min(cur_col, math.max(1, string.len(lines[cur_line])))
                sync_to_text(lines)
                last_notepad_text = _G.notepad_text
                pending_key = nil
            else
                pending_key = "d"
            end
        elseif char == "p" then
            if yanked_line then
                push_undo(lines)
                table.insert(lines, cur_line + 1, yanked_line)
                cur_line = cur_line + 1
                cur_col = 1
                sync_to_text(lines)
                last_notepad_text = _G.notepad_text
            end
        elseif char == ":" then
            mode = "COMMAND"
            command_text = ""
        elseif char == "u" then
            local prev_lines = pop_undo()
            if prev_lines then
                lines = prev_lines
                if cur_line > #lines then cur_line = #lines end
                cur_col = math.min(cur_col, math.max(1, string.len(lines[cur_line])))
                sync_to_text(lines)
                last_notepad_text = _G.notepad_text
                status_message = "Undo applied"
                status_timer = getUptime() + 2
            else
                status_message = "No older changes"
                status_timer = getUptime() + 2
            end
        else
            pending_key = nil
        end

    elseif mode == "COMMAND" then
        if key == 1 or key == KEY_ESC then
            mode = "NORMAL"
        elseif key == 14 or key == KEY_BACKSPACE then
            if string.len(command_text) > 0 then
                command_text = string.sub(command_text, 1, -2)
            else
                mode = "NORMAL"
            end
        elseif key == 28 or key == KEY_ENTER then
            if command_text == "w" then
                do_save("NOTES.TXT")
            elseif command_text == "q" then
                _G.notepad_text = ""
                lines = {""}
                cur_line = 1
                cur_col = 1
                scroll_offset = 0
            elseif command_text == "wq" then
                do_save("NOTES.TXT")
                _G.notepad_text = ""
                lines = {""}
                cur_line = 1
                cur_col = 1
                scroll_offset = 0
            else
                status_message = "Unknown command"
                status_timer = getUptime() + 3
            end
            mode = "NORMAL"
        elseif string.len(char) > 0 and string.byte(char) >= 32 then
            command_text = command_text .. char
        end
    end
end

return M
--- END OF FILE res/sysgui/notepad.lua ---