-- res/sysgui/terminal.lua
local term_lines = {
    "Equinox OS Ring 3 Terminal [Version 2.0]",
    "Welcome to the Lua-driven CLI terminal shell.",
    "Type 'help' to see available commands.",
    ""
}

-- Экспортируем состояния для глобального цикла ввода
_G.term_lines = term_lines
_G.term_input = ""

local matrix_mode = false
local matrix_tick = 0
local last_blink_state = -1

local function strip_ansi(s)
    if not s then return "" end
    return (s:gsub("\27%[[%d;]*m", ""))
end

local function term_append_multiline(text)
    text = strip_ansi(text or "")
    if text == "" then return end
    for line in (text .. "\n"):gmatch("([^\n]*)\n") do
        table.insert(term_lines, line)
    end
end

local function process_command(raw)
    raw = raw or ""
    local s = string.match(raw, "^%s*(.-)%s*$") or ""
    if s == "" then return end

    local verb = string.match(s, "^(%S+)")

    local GUI_LOCAL_COMMANDS = {
        clear   = function() 
            _G.term_lines = {}
            term_lines = _G.term_lines
        end,
        matrix  = function()
            matrix_mode = not matrix_mode
            table.insert(term_lines, "Matrix digital rain: " .. (matrix_mode and "ENABLED" or "DISABLED"))
        end,
        doom    = function()
            table.insert(term_lines, "Launching doom.elf...")
            exec("bin/doom.elf -iwad res/doom1.wad")
        end,
        snake   = function()
            table.insert(term_lines, "Launching snake.elf...")
            exec("bin/snake.elf")
        end,
    }

    local local_handler = GUI_LOCAL_COMMANDS[verb]
    if local_handler then
        local_handler()
        return
    end

    if type(shellExec) ~= "function" then
        table.insert(term_lines, "shellExec() not available — rebuild sysgui against new kernel")
        return
    end

    local out = shellExec(s)
    term_append_multiline(out)
end

_G.process_terminal_command = process_command

local function draw_terminal(win, mx, my, mdown, dt)
    if matrix_mode then
        matrix_tick = matrix_tick + 1
        _G.needs_redraw = true
        drawRect(win.x, win.y, win.w, win.h, 0x000000)
        for i = 1, 30 do
            local rx = win.x + ((i * 17) % win.w)
            local speed = 2 + (i % 4)
            local ry = win.y + math.floor((matrix_tick * speed + i * 43) % win.h)
            local char_code = 33 + ((matrix_tick + i) % 90)
            local char_str = string.char(char_code)
            local col = (i % 5 == 0) and 0xFFFFFF or 0x00FF00
            drawText(char_str, rx, ry, col)
        end
    end

    local line_h = 14
    local max_lines = math.floor((win.h - 30) / line_h)
    local start_idx = #term_lines - max_lines + 1
    if start_idx < 1 then start_idx = 1 end

    local draw_y = win.y + 8
    for i = start_idx, #term_lines do
        if term_lines[i] then
            drawText(term_lines[i], win.x + 8, draw_y, 0x50FA7B)
            draw_y = draw_y + line_h
        end
    end

    local prompt_y = win.y + win.h - 22
    drawRect(win.x, prompt_y, win.w, 1, 0x2E3440)
    drawText(">> " .. _G.term_input, win.x + 8, prompt_y + 4, 0xF8F8F2)

    local blink = math.floor(getUptime() * 2) % 2
    if blink ~= last_blink_state then
        _G.needs_redraw = true
        last_blink_state = blink
    end
    if blink == 0 then
        local cur_cx = win.x + 8 + 24 + string.len(_G.term_input) * 8
        drawRect(cur_cx, prompt_y + 16, 8, 2, 0x8BE9FD)
    end
end

return draw_terminal