--- START OF FILE res/sysgui/terminal.lua ---
local M = {}

-- Этап 10: GUI terminal больше не делает вид, что он сам шелл. Системный
-- шелл живёт в /bin/sh.elf (bash). Здесь — лёгкий «GUI debug terminal»,
-- который понимает несколько GUI-локальных команд (для удобства из окна)
-- и всё остальное молча скармливает в ring-0 shell через shellExec().
local term_lines = {
    "EquinoxOS — GUI debug terminal",
    "System shell: /bin/sh.elf (bash). Type 'sh' to launch it in console.",
    "GUI-local: help, neofetch, ls, cat <f>, ps, kill <pid>, clear, doom, snake.",
    ""
}

_G.term_lines = term_lines
_G.term_input = ""

local last_blink_state = -1
local cmd_history = {}
local history_idx = 0
local scroll_offset = 0

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

local function execute_cli_command(raw_input)
    local s = string.match(raw_input, "^%s*(.-)%s*$") or ""
    if s == "" then return end

    local parts = {}
    for word in s:gmatch("%S+") do
        table.insert(parts, word)
    end
    local verb = parts[1]

    -- Shell Utilities Implementation
    if verb == "help" then
        term_append_multiline("EquinoxOS — GUI terminal\n" ..
                             "  sh            Launch /bin/sh.elf system shell (COM1 console)\n" ..
                             "  neofetch      System info\n" ..
                             "  ls            List files on Root VFS\n" ..
                             "  cat <file>    Print file content\n" ..
                             "  rm <file>     Delete file from storage\n" ..
                             "  clear         Clear console log\n" ..
                             "  ps            List running processes\n" ..
                             "  kill <pid>    Terminate process\n" ..
                             "  doom / snake  Launch system games\n" ..
                             "  <anything>    Forwarded to kernel shell\n")
        return
    end

    if verb == "neofetch" or verb == "sysinfo" then
        local used, total = getMemInfo()
        local sw, sh = getScreenSize()
        local sys_art = "\n" ..
            "  #######   Equinox OS Ring 3\n" ..
            " #######    -----------------\n" ..
            " ##         Kernel: Ring-0 Micro Monolithic\n" ..
            " ##         Shell: enGUI Modular Interpreter\n" ..
            " #######    Resolution: " .. sw .. "x" .. sh .. "\n" ..
            "  #######   Memory: " .. math.floor(used/(1024*1024)) .. " / " .. math.floor(total/(1024*1024)) .. " MB\n" ..
            "            Uptime: " .. math.floor(getUptime()) .. " seconds\n"
        term_append_multiline(sys_art)
        return
    end

    if verb == "ls" then
        local ok, files = pcall(getFiles)
        if ok and type(files) == "table" then
            local file_str = "VFS Root Directory Layout:\n"
            for _, f in ipairs(files) do
                file_str = file_str .. string.format("  %-18s  [%dB]  dev: %s\n", f.name, f.size, f.dev)
            end
            term_append_multiline(file_str)
        else
            term_append_multiline("VFS read failure.")
        end
        return
    end

    if verb == "cat" then
        local name = parts[2]
        if not name then term_append_multiline("Syntax: cat <filename>") return end
        local content = readFile(name)
        if content then
            term_append_multiline(content)
        else
            term_append_multiline("File not found.")
        end
        return
    end

    if verb == "rm" then
        local name = parts[2]
        if not name then term_append_multiline("Syntax: rm <filename>") return end
        saveFile(name, "") -- Empty file functions as VFS removal placeholder
        if type(_G.refresh_explorer) == "function" then _G.refresh_explorer() end
        term_append_multiline("File wiped successfully.")
        return
    end

    if verb == "ps" then
        local ok, tasks = pcall(getTasks)
        if ok and type(tasks) == "table" then
            local task_str = "Active Tasks:\n"
            for _, t in ipairs(tasks) do
                task_str = task_str .. string.format("  PID: %d  State: %s  CR3: 0x%X\n", t.pid, t.state, t.cr3)
            end
            term_append_multiline(task_str)
        else
            term_append_multiline("Task manager read error.")
        end
        return
    end

    if verb == "kill" then
        local pid = tonumber(parts[2])
        if not pid then term_append_multiline("Syntax: kill <pid>") return end
        local ok = killTask(pid)
        term_append_multiline(ok and "Process terminated." or "Process termination failure.")
        return
    end

    if verb == "clear" then
        _G.term_lines = {}
        term_lines = _G.term_lines
        return
    end

    if verb == "doom" then
        term_append_multiline("Launching doom.elf...")
        exec("bin/doom.elf -iwad res/doom1.wad")
        return
    elseif verb == "snake" then
        term_append_multiline("Launching snake.elf...")
        exec("bin/snake.elf")
        return
    elseif verb == "sh" then
        -- Системный шелл — это ring-3 ELF, его I/O висит на COM1 (см. sh.elf
        -- BUILD_NOTES). Сюда, в GUI-окно, его вывод не пойдёт — пользователь
        -- увидит сессию через серийную консоль. Поэтому подсказываем, и
        -- запускаем процесс через exec().
        term_append_multiline("Launching /bin/sh.elf — session goes to COM1 serial console.")
        exec("bin/sh.elf")
        return
    end

    if type(shellExec) == "function" then
        local out = shellExec(s)
        term_append_multiline(out)
    else
        term_append_multiline("Unknown shell command. Type 'help'.")
    end
end

M.draw = function(win, mx, my, mdown, dt)
    local line_h = 14
    local max_lines = math.floor((win.h - 30) / line_h)
    
    local total_lines = #term_lines
    local end_idx = total_lines - scroll_offset
    local start_idx = end_idx - max_lines + 1
    if start_idx < 1 then start_idx = 1 end

    local draw_y = win.y + 8
    for i = start_idx, end_idx do
        if term_lines[i] then
            drawText(term_lines[i], win.x + 8, draw_y, 0x98C379)
            draw_y = draw_y + line_h
        end
    end

    local prompt_y = win.y + win.h - 22
    drawRect(win.x, prompt_y, win.w, 1, 0x3E4452)
    
    local input_prefix = ">> "
    if scroll_offset > 0 then
        input_prefix = string.format("[%d] >> ", scroll_offset)
    end
    drawText(input_prefix .. _G.term_input, win.x + 8, prompt_y + 4, 0xABB2BF)

    local blink = math.floor(getUptime() * 2) % 2
    if blink ~= last_blink_state then
        _G.needs_redraw = true
        last_blink_state = blink
    end
    if blink == 0 then
        local cur_cx = win.x + 8 + (string.len(input_prefix) + string.len(_G.term_input)) * 8
        drawRect(cur_cx, prompt_y + 16, 8, 2, 0x61AFEF)
    end
end

M.handle_key = function(win, key, char)
    if key == KEY_ENTER then
        if _G.term_input ~= "" then
            table.insert(cmd_history, _G.term_input)
            history_idx = #cmd_history + 1
        end
        table.insert(term_lines, ">> " .. _G.term_input)
        execute_cli_command(_G.term_input)
        _G.term_input = ""
        scroll_offset = 0

    elseif key == KEY_BACKSPACE then
        _G.term_input = string.sub(_G.term_input, 1, -2)

    elseif key == KEY_TAB then
        local word = string.match(_G.term_input, "(%S+)$") or ""
        if word ~= "" then
            local ok, files = pcall(getFiles)
            if ok and type(files) == "table" then
                local wlen = string.len(word)
                for i = 1, math.min(#files, 128) do
                    local f = files[i]
                    if f and f.name and string.sub(f.name, 1, wlen) == word then
                        _G.term_input = _G.term_input .. string.sub(f.name, wlen + 1)
                        break
                    end
                end
            end
        end

    elseif key == KEY_UP then
        if #cmd_history > 0 then
            history_idx = history_idx - 1
            if history_idx < 1 then history_idx = 1 end
            _G.term_input = cmd_history[history_idx] or ""
        end

    elseif key == KEY_DOWN then
        if #cmd_history > 0 then
            history_idx = history_idx + 1
            if history_idx > #cmd_history then
                history_idx = #cmd_history + 1
                _G.term_input = ""
            else
                _G.term_input = cmd_history[history_idx] or ""
            end
        end

    elseif key == KEY_PGUP then
        scroll_offset = scroll_offset + 3
        local max_lines = math.floor((win.h - 30) / 14)
        if scroll_offset > #term_lines - max_lines then
            scroll_offset = #term_lines - max_lines
        end
        if scroll_offset < 0 then scroll_offset = 0 end

    elseif key == KEY_PGDN then
        scroll_offset = scroll_offset - 3
        if scroll_offset < 0 then scroll_offset = 0 end

    elseif key == KEY_HOME then
        local max_lines = math.floor((win.h - 30) / 14)
        scroll_offset = math.max(0, #term_lines - max_lines)

    elseif key == KEY_END then
        scroll_offset = 0

    elseif string.len(char) > 0 and string.byte(char) >= 32 then
        _G.term_input = _G.term_input .. char
    end
end

return M
--- END OF FILE res/sysgui/terminal.lua ---