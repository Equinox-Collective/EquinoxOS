-- Equinox Desktop Environment (enGUI)
print("enGUI Desktop Environment: Windows & Apps loaded!")

local windows = {}
local focused_window = nil
local last_mdown = false
local resizing_win = nil

-- Флаг грязного кадра: Lua выставляет в true, если нужна перерисовка
-- (matrix mode, мигающий курсор). C читает через getLastKey() side-effect.
_G.needs_redraw = false

-- Состояние глобального ввода для Блокнота / Терминала
local shift_pressed = false
-- Кэш последнего значения blink для отслеживания переходов
local last_blink_state = -1

-- ---КЛАСС WINDOW (ООП МЕНЕДЖЕР ОКОН) ---
local Window = {}
Window.__index = Window

function Window.new(title, x, y, w, h, draw_cb)
    local self = setmetatable({}, Window)
    self.title = title
    self.x = x
    self.y = y
    self.w = w
    self.h = h
    self.active = true
    self.borderless = false   -- Флаг: без рамок и заголовка
    self.fullscreen = false   -- Флаг: на весь экран
    self.is_app_container = false
    self.draw_cb = draw_cb
    return self
end

function Window:draw(mx, my, mdown, dt)
    if not self.active then return end

    local sw, sh = getScreenSize()
    
    -- Обработка Fullscreen состояния
    if self.fullscreen then
        self.x, self.y = 0, 0
        self.w, self.h = sw, sh
    end

    local active = (focused_window == self)
    
    -- РИСУЕМ РАМКИ только если окно НЕ borderless и НЕ fullscreen
    if not self.borderless and not self.fullscreen then
        local ty = self.y - 28
        -- Тень
        drawRect(self.x + 2, ty + 2, self.w + 4, self.h + 30, 0x000000AA) 
        -- Обводка
        drawRect(self.x - 1, ty - 1, self.w + 2, self.h + 30, active and 0x555555 or 0x333333)
        -- Заголовок
        if active then
            drawGradient(self.x, ty, self.w, 28, 0x1A72BB, 0x0E4581, true)
        else
            drawGradient(self.x, ty, self.w, 28, 0x2D2D30, 0x1E1E1E, true)
        end
        -- Текст и кнопка закрытия
        drawText(self.title, self.x + 10, ty + 8, 0xFFFFFF)
        local bx = self.x + self.w - 26
        if button("X", bx, ty + 4, 22, 20) then
            self.active = false
            return
        end
        -- Разделитель
        drawRect(self.x, self.y - 1, self.w, 1, active and 0x55AFFF or 0x444444)
    end

    -- РЕНДЕРИНГ КОНТЕНТА
    if not self.is_app_container then
        -- Обычное окно (если borderless, можем сделать его прозрачным, не рисуя фон)
        if not self.borderless then
            drawRect(self.x, self.y, self.w, self.h, 0x1E1E1E)
        end
        
        if self.draw_cb then 
            self.draw_cb(self, mx, my, mdown, dt) 
        end
    else
        -- Контейнер для эльфа (Doom/Snake)
        if type(setAppWindowPos) == "function" then
            setAppWindowPos(self.x, self.y, self.w, self.h)
        end
    end

    -- Resize Handle (только для обычных окон)
    if not self.borderless and not self.fullscreen then
        local rx, ry = self.x + self.w - 10, self.y + self.h - 10
        drawRect(rx, ry, 10, 10, active and 0x0078D7 or 0x444444)
        if mdown and not last_mdown and mx >= rx and mx < rx+10 and my >= ry and my < ry+10 then
            resizing_win = self
        end
    end
end

-- --- ПРИЛОЖЕНИЕ 1: EQUINOX TERMINAL ---
local term_lines = {
    "Equinox OS Ring 3 Terminal [Version 2.0]",
    "Welcome to the Lua-driven CLI terminal shell.",
    "Type 'help' to see available commands.",
    ""
}
local term_input = ""
local matrix_mode = false
local matrix_tick = 0

-- ANSI-эскейпы (\e[33m ... \e[0m), которые kernel-shell аккуратно
-- расставляет для help/ps, мы тут просто отрезаем — у нашего терминала
-- нет цветного рендера, иначе они вылезают мусором.
local function strip_ansi(s)
    if not s then return "" end
    -- \027 = ESC. Паттерн ловит ESC[...m любой длины.
    return (s:gsub("\27%[[%d;]*m", ""))
end

-- Добавляет многострочный текст в term_lines, разбивая по '\n'.
local function term_append_multiline(text)
    text = strip_ansi(text or "")
    if text == "" then return end
    for line in (text .. "\n"):gmatch("([^\n]*)\n") do
        table.insert(term_lines, line)
    end
end

-- Команды, которые ВСЕГДА обрабатываем сами (т.к. они GUI-specific и в
-- kernel-shell их и не должно быть).
local GUI_LOCAL_COMMANDS = {
    clear   = function() term_lines = {} end,
    matrix  = function()
        matrix_mode = not matrix_mode
        table.insert(term_lines,
            "Matrix digital rain: " .. (matrix_mode and "ENABLED" or "DISABLED"))
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

local function process_command(raw)
    raw = raw or ""
    local s = string.match(raw, "^%s*(.-)%s*$") or ""
    if s == "" then return end

    local verb = string.match(s, "^(%S+)")

    -- 1) GUI-локальные команды (clear/matrix/doom/snake) — без syscall.
    local local_handler = GUI_LOCAL_COMMANDS[verb]
    if local_handler then
        local_handler()
        return
    end

    -- 2) Всё остальное (help/ps/kill/killall/run/ls/fetch/reboot/...) —
    --    отдаём ring-0 shell через SYS_SHELL_EXEC. Если binding по
    --    какой-то причине отсутствует (старый kernel), оставляем
    --    короткий понятный fallback.
    if type(shellExec) ~= "function" then
        table.insert(term_lines,
            "shellExec() not available — rebuild sysgui against new kernel")
        return
    end

    local out = shellExec(s)
    term_append_multiline(out)
end

local function draw_terminal(win, mx, my, mdown, dt)
    -- Обработка Matrix-режима
    if matrix_mode then
        matrix_tick = matrix_tick + 1
        _G.needs_redraw = true  -- требуем перерисовку каждый кадр
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

    -- Рендеринг строк истории вывода
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

    -- Нижний разделитель строки ввода
    local prompt_y = win.y + win.h - 22
    drawRect(win.x, prompt_y, win.w, 1, 0x2E3440)
    drawText(">> " .. term_input, win.x + 8, prompt_y + 4, 0xF8F8F2)

    -- Мигающий курсор терминала (только отмечаем dirty при смене фазы)
    local blink = math.floor(getUptime() * 2) % 2
    if blink ~= last_blink_state then
        _G.needs_redraw = true
        last_blink_state = blink
    end
    if blink == 0 then
        local cur_cx = win.x + 8 + 24 + string.len(term_input) * 8
        drawRect(cur_cx, prompt_y + 16, 8, 2, 0x8BE9FD)
    end
end

-- --- ПРИЛОЖЕНИЕ 2: SYSTEM MONITOR ---
local function draw_monitor(win, mx, my, mdown, dt)
    local used, total = getMemInfo()
    local used_mb = math.floor(used / (1024 * 1024))
    local total_mb = math.floor(total / (1024 * 1024))

    local ram_str = string.format("System RAM: %d / %d MB", used_mb, total_mb)
    drawText(ram_str, win.x + 15, win.y + 20, 0xE5E9F0)

    -- Фоновая полоса
    drawRect(win.x + 15, win.y + 40, win.w - 30, 12, 0x2E3440)
    -- Заполненная полоса
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

-- --- ПРИЛОЖЕНИЕ 3: PAINT VECTOR DRAW ---
local paint_strokes = {} -- Список мазков
local active_stroke = nil
local active_color = 0xFF0000

local palette = {
    0x000000, 0xFF0000, 0x00FF00, 0x0000FF,
    0xFFFF00, 0xFF00FF, 0x00FFFF, 0xFFFFFF
}

local function draw_paint(win, mx, my, mdown, dt)
    -- Панель инструментов
    drawRect(win.x, win.y, win.w, 24, 0x2E303B)
    drawRect(win.x, win.y + 24, win.w, 1, 0x4B5263)

    -- Палитра
    for idx, col in ipairs(palette) do
        local px = win.x + 4 + (idx - 1) * 22
        drawRect(px, win.y + 3, 18, 18, col)
        
        -- Выбор цвета кликом по палитре
        if mx >= px and mx < px + 18 and my >= win.y + 3 and my < win.y + 21 then
            if mdown and not last_mdown then
                active_color = col
            end
        end
    end

    -- Кнопка "Очистить" (CLR)
    local clr_x = win.x + win.w - 110
    if button("CLR", clr_x, win.y + 3, 45, 18) then
        paint_strokes = {}
    end

    -- Кнопка "Сохранить" (SAVE)
    local save_x = win.x + win.w - 55
    if button("SAVE", save_x, win.y + 3, 50, 18) then
        saveFile("PAINT.TXT", "Equinox Paint Canvas Dump")
    end

    -- Обработка рисования (холст ниже панели инструментов).
    -- Важно: рисовать можно ТОЛЬКО если Paint в фокусе И клик СТАРТОВАЛ
    -- внутри холста. Иначе перетаскивание любого другого окна поверх
    -- Paint вызывает срабатывание этого хэндлера: mdown=true, mx/my
    -- внутри Paint, и мы оставляли линии "под чужим окном". Теперь
    -- штрих стартуем только на edge (mdown && !last_mdown) при фокусе
    -- на Paint, продолжаем — пока мышь зажата и штрих активен.
    local canvas_y = win.y + 25
    local inside_canvas = (mx >= win.x and mx < win.x + win.w
                           and my >= canvas_y and my < win.y + win.h)
    local is_focused = (focused_window == win)

    if is_focused and inside_canvas and mdown and not last_mdown then
        active_stroke = { color = active_color, points = {{ x = mx, y = my }} }
        table.insert(paint_strokes, active_stroke)
    elseif active_stroke and mdown then
        table.insert(active_stroke.points, { x = mx, y = my })
    end
    if not mdown then
        active_stroke = nil
    end

    -- Рендеринг всех нарисованных мазков (только внутри холста, иначе
    -- линии "вытекали" за рамку окна при сдвиге Paint к краю экрана).
    local canvas_x0 = win.x
    local canvas_y0 = canvas_y
    local canvas_x1 = win.x + win.w
    local canvas_y1 = win.y + win.h
    local function in_canvas(p)
        return p.x >= canvas_x0 and p.x < canvas_x1
           and p.y >= canvas_y0 and p.y < canvas_y1
    end
    for _, stroke in ipairs(paint_strokes) do
        local points = stroke.points
        for k = 1, #points - 1 do
            local p1 = points[k]
            local p2 = points[k+1]
            if in_canvas(p1) and in_canvas(p2) then
                drawLine(p1.x, p1.y, p2.x, p2.y, stroke.color)
            end
        end
    end
end

-- --- ПРИЛОЖЕНИЕ 4: FILE EXPLORER ---
local files_list = {}
local function refresh_explorer()
    files_list = getFiles()
end

local function draw_explorer(win, mx, my, mdown, dt)
    -- Панель путей
    drawRect(win.x, win.y, win.w, 24, 0x2E303B)
    drawText("Root VFS Volume Directory", win.x + 8, win.y + 6, 0xD8DEE9)

    -- Кнопка обновления REFR
    if button("REFR", win.x + win.w - 55, win.y + 3, 50, 18) then
        refresh_explorer()
    end

    local draw_y = win.y + 32
    if #files_list == 0 then
        drawText("No volumes or files found.", win.x + 20, draw_y, 0x888C94)
    end

    -- Сколько строк влезает в окно (не считая шапку 32px). Раньше
    -- рисовали все элементы списка подряд → они вытекали ниже окна и
    -- "висели" на рабочем столе. И длинные имена накладывались на
    -- колонку EXT2_DISK, потому что текст не клипался.
    local row_h = 22
    local max_rows = math.max(0, math.floor((win.h - 32) / row_h))
    local dev_col_x = win.x + win.w - 120
    local name_col_x = win.x + 26
    local max_name_chars = math.max(0, math.floor((dev_col_x - name_col_x - 4) / 8))

    for idx = 1, math.min(#files_list, max_rows) do
        local f = files_list[idx]
        local row_y = draw_y + (idx - 1) * row_h
        local is_hover = (mx >= win.x and mx < win.x + win.w and my >= row_y and my < row_y + 20)

        -- Эффект выделения строки
        if is_hover then
            drawRect(win.x, row_y, win.w, 20, 0x333644)
        end

        -- Значок диска (EXT2 синий, FAT желтый)
        local icon_col = (f.dev == "EXT2_DISK") and 0x5E81AC or 0xEBCB8B
        drawRect(win.x + 8, row_y + 5, 10, 10, icon_col)

        -- Название (обрезанное под колонку) и dev
        local display_name = f.name or ""
        if #display_name > max_name_chars and max_name_chars > 1 then
            display_name = string.sub(display_name, 1, max_name_chars - 1) .. "~"
        end
        drawText(display_name, name_col_x, row_y + 3, 0xE5E9F0)
        drawText(f.dev, dev_col_x, row_y + 3, 0x888C94)

        -- При клике открываем файл в Блокноте
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
                -- Считываем контент
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

-- --- ПРИЛОЖЕНИЕ 5: NOTEPAD TEXT EDITOR ---
_G.notepad_text = "This is a simple text document.\nYou can write text here using your keyboard."

local function draw_notepad(win, mx, my, mdown, dt)
    -- Панель Блокнота
    drawRect(win.x, win.y, win.w, 24, 0x2E303B)
    drawText("NOTES.TXT", win.x + 8, win.y + 6, 0xE5E9F0)

    -- Кнопка сохранения SAVE
    if button("SAVE", win.x + win.w - 55, win.y + 3, 50, 18) then
        saveFile("NOTES.TXT", _G.notepad_text)
        refresh_explorer()
    end

    -- Выводим текст построчно
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

    -- Курсор ввода текста на последней строке (dirty только при смене фазы)
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

-- --- ИНИЦИАЛИЗАЦИЯ ИКОНОК И ОКОН ---
-- 44x44 Doom-cover icon, quantized to 12 colors, run-length encoded per row.
-- Each row table is a flat sequence of (color, run_length) pairs.
local doom_pixels = {
    {0xC3130B,5,0x763605,14,0xC3130B,2,0x763605,1,0xC3130B,1,0x763605,21},
    {0xC3130B,4,0x763605,17,0xC3130B,2,0x763605,21},
    {0xC3130B,3,0x763605,3,0x975756,1,0xBA9B9A,2,0xEACBC7,4,0xBA9B9A,1,0x975756,1,0xEACBC7,5,0xBA9B9A,1,0x763605,1,0x975756,1,0xEACBC7,5,0xBA9B9A,2,0xEACBC7,2,0x763605,1,0x975756,1,0xEACBC7,1,0xBA9B9A,2,0x763605,7},
    {0xC3130B,3,0x763605,4,0xEACBC7,1,0xFFFFFF,6,0xEACBC7,1,0xFFFFFF,6,0xBA9B9A,1,0xEACBC7,1,0xFFFFFF,6,0xEACBC7,1,0xFFFFFF,2,0x975756,1,0xBA9B9A,1,0xFFFFFF,2,0x975756,1,0x763605,7},
    {0xC3130B,2,0x763605,5,0xEACBC7,1,0xFFFFFF,1,0xEACBC7,1,0x975756,1,0xEACBC7,1,0xFFFFFF,2,0xEACBC7,1,0xFFFFFF,2,0xBA9B9A,2,0xFFFFFF,2,0xEACBC7,2,0xFFFFFF,1,0xEACBC7,1,0xBA9B9A,1,0xEACBC7,1,0xFFFFFF,1,0xEACBC7,2,0xFFFFFF,2,0xEACBC7,2,0xFFFFFF,2,0x975756,1,0x763605,7},
    {0xC3130B,2,0x763605,3,0xC3130B,2,0xEACBC7,1,0xFFFFFF,1,0xEACBC7,1,0x763605,1,0x975756,1,0xFFFFFF,1,0xEACBC7,2,0xFFFFFF,2,0x763605,2,0xFFFFFF,2,0xEACBC7,2,0xFFFFFF,1,0xBA9B9A,1,0x763605,1,0xBA9B9A,1,0xFFFFFF,1,0xEACBC7,2,0xFFFFFF,6,0x975756,1,0x763605,7},
    {0xC3130B,2,0x763605,2,0xC3130B,3,0xEACBC7,1,0xFFFFFF,1,0xEACBC7,1,0xC3130B,1,0x975756,1,0xFFFFFF,1,0xEACBC7,2,0xFFFFFF,2,0x975756,1,0x763605,1,0xFFFFFF,2,0xEACBC7,2,0xFFFFFF,1,0xBA9B9A,1,0x763605,1,0xBA9B9A,1,0xFFFFFF,1,0xEACBC7,2,0xFFFFFF,6,0x975756,1,0x763605,7},
    {0xC3130B,2,0x763605,2,0xC3130B,3,0xEACBC7,1,0xFFFFFF,1,0xEACBC7,1,0x763605,1,0x975756,1,0xFFFFFF,2,0xEACBC7,1,0xFFFFFF,2,0x975756,1,0x763605,1,0xFFFFFF,2,0xEACBC7,2,0xFFFFFF,1,0xBA9B9A,1,0x763605,1,0xBA9B9A,1,0xFFFFFF,1,0xEACBC7,2,0xFFFFFF,6,0x975756,1,0x763605,7},
    {0xC3130B,7,0xEACBC7,1,0xFFFFFF,1,0xEACBC7,1,0x763605,1,0x975756,1,0xFFFFFF,2,0xEACBC7,1,0xFFFFFF,2,0xC3130B,2,0xFFFFFF,2,0xEACBC7,2,0xFFFFFF,1,0xBA9B9A,1,0x763605,1,0xBA9B9A,1,0xFFFFFF,1,0xEACBC7,2,0xFFFFFF,6,0x975756,1,0x763605,7},
    {0x763605,1,0xC3130B,6,0xEACBC7,1,0xFFFFFF,1,0xEACBC7,1,0x763605,1,0x975756,1,0xFFFFFF,2,0xEACBC7,1,0xFFFFFF,2,0x975756,1,0xC3130B,1,0xEACBC7,1,0xFFFFFF,1,0xEACBC7,2,0xFFFFFF,1,0xBA9B9A,1,0x763605,1,0xBA9B9A,1,0xFFFFFF,1,0xEACBC7,2,0xFFFFFF,6,0x975756,1,0x763605,7},
    {0xC3130B,7,0xEACBC7,1,0xFFFFFF,1,0xEACBC7,1,0x975756,1,0xFFFFFF,3,0xEACBC7,1,0xFFFFFF,2,0xEACBC7,1,0xCC7A38,1,0xEACBC7,1,0xFFFFFF,1,0xEACBC7,2,0xFFFFFF,1,0xBA9B9A,2,0xFFFFFF,3,0xEACBC7,1,0xFFFFFF,1,0xEACBC7,3,0xFFFFFF,2,0x975756,1,0x763605,1,0xC3130B,3,0x763605,3},
    {0xC3130B,7,0xEACBC7,1,0xFFFFFF,1,0xEACBC7,1,0xFFFFFF,4,0x975756,1,0xBA9B9A,1,0xFFFFFF,5,0xEACBC7,1,0xFFFFFF,6,0x975756,1,0xBA9B9A,1,0xFFFFFF,1,0xEACBC7,1,0xBA9B9A,2,0xFFFFFF,2,0x975756,1,0x763605,1,0xC3130B,1,0x763605,5},
    {0xC3130B,6,0x763605,1,0xEACBC7,1,0xFFFFFF,1,0xEACBC7,2,0xFFFFFF,1,0xEACBC7,1,0x975756,1,0x763605,2,0x975756,1,0xFFFFFF,4,0xBA9B9A,1,0xEACBC7,1,0xFFFFFF,3,0xEACBC7,1,0x975756,1,0xC3130B,2,0xBA9B9A,1,0xEACBC7,1,0xC3130B,1,0x975756,1,0xFFFFFF,2,0x975756,1,0xC3130B,2,0x763605,5},
    {0xC3130B,5,0x763605,2,0xBA9B9A,1,0xFFFFFF,1,0xEACBC7,2,0x975756,1,0x763605,4,0xC3130B,1,0x763605,1,0xBA9B9A,2,0x975756,1,0xC3130B,2,0xBA9B9A,2,0xCC7A38,1,0xC3130B,8,0xEACBC7,1,0xFFFFFF,1,0x975756,1,0x763605,1,0xC3130B,1,0x763605,4,0xC3130B,1},
    {0xF45108,2,0xC3130B,1,0x763605,2,0xC3130B,2,0xEACBC7,1,0xFFFFFF,1,0xEACBC7,1,0xC3130B,3,0x763605,1,0xC3130B,1,0x763605,6,0xC3130B,13,0xEACBC7,1,0xFFFFFF,1,0x975756,1,0xC3130B,3,0x763605,2,0xC3130B,1,0x763605,1},
    {0xF45108,1,0x763605,6,0xBA9B9A,2,0xC3130B,5,0x763605,1,0xC3130B,1,0x763605,1,0xC3130B,2,0x763605,2,0xC3130B,14,0xEACBC7,1,0x975756,1,0xC3130B,5,0x763605,2},
    {0xC3130B,1,0x763605,1,0x290002,1,0x763605,2,0xC3130B,8,0x763605,1,0xC3130B,3,0x763605,1,0xC3130B,2,0x763605,1,0xC3130B,2,0x763605,3,0xC3130B,18},
    {0xC3130B,2,0x763605,3,0xC3130B,1,0xF45108,1,0xC3130B,9,0x763605,1,0xC3130B,3,0x763605,1,0xC3130B,2,0x763605,6,0xC3130B,15},
    {0xC3130B,1,0xF45108,1,0xC3130B,1,0x763605,1,0xC3130B,2,0xF45108,1,0xC3130B,6,0xF45108,1,0xC3130B,7,0x763605,1,0xC3130B,2,0x763605,8,0xC3130B,4,0x763605,3,0xC3130B,5},
    {0xC3130B,2,0xF45108,1,0x763605,1,0xC3130B,2,0xF45108,1,0xC3130B,2,0x763605,1,0xC3130B,2,0xF45108,4,0xC3130B,2,0xF45108,3,0xC3130B,3,0x763605,12,0xC3130B,7,0x763605,1},
    {0xC3130B,2,0xCC7A38,3,0xF45108,1,0xC3130B,1,0x763605,2,0xC3130B,1,0xF45108,10,0xCC7A38,2,0xF45108,1,0xCC7A38,1,0x763605,4,0xC3130B,3,0x763605,5,0xC3130B,6,0x763605,2},
    {0xC3130B,2,0x763605,1,0xC3130B,2,0xCC7A38,1,0x763605,1,0x975756,1,0xCC7A38,1,0xF45108,3,0xEEA50E,1,0xF45108,1,0xC3130B,1,0xF45108,3,0xEEA50E,1,0xCC7A38,1,0x763605,3,0x9A8A0B,2,0x763605,3,0xC3130B,3,0x975756,1,0x763605,1,0xCC7A38,2,0x763605,2,0xC3130B,1,0xF45108,1,0xC3130B,3,0x763605,2},
    {0xC3130B,2,0x763605,4,0xC3130B,3,0xCC7A38,4,0xF45108,1,0xC3130B,2,0xF45108,2,0xEEA50E,1,0x9A8A0B,4,0x763605,2,0xC3130B,5,0xCC7A38,1,0xEEA50E,1,0x763605,3,0xC3130B,2,0xCC7A38,2,0xC3130B,2,0x763605,3},
    {0xC3130B,3,0x763605,2,0xC3130B,2,0x763605,1,0xC3130B,1,0xF45108,1,0xFDDA5C,1,0xCC7A38,1,0xC3130B,2,0xF45108,3,0xCC7A38,1,0x763605,2,0x9A8A0B,3,0x763605,1,0xF45108,2,0xC3130B,2,0xF45108,1,0xEEA50E,1,0xC3130B,1,0xCC7A38,3,0xF45108,1,0xC3130B,2,0xCC7A38,2,0xC3130B,2,0x763605,3},
    {0xC3130B,1,0xCC7A38,1,0x763605,7,0xC3130B,1,0xF45108,4,0x763605,1,0xCC7A38,1,0xEEA50E,1,0x9A8A0B,1,0x763605,6,0xCC7A38,1,0xF45108,2,0xEEA50E,2,0xC3130B,2,0xCC7A38,2,0xF45108,1,0xCC7A38,1,0xF45108,1,0xCC7A38,2,0xC3130B,2,0x763605,2,0xC3130B,2},
    {0xC3130B,2,0x763605,3,0xC3130B,1,0x763605,4,0xCC7A38,1,0xC3130B,1,0xF45108,1,0xEEA50E,1,0xF45108,1,0x763605,2,0x9A8A0B,1,0x763605,6,0xF45108,4,0xEEA50E,3,0xCC7A38,1,0xF45108,4,0xC3130B,4,0x763605,3,0xC3130B,1},
    {0xC3130B,2,0x763605,1,0x290002,1,0x763605,6,0xCC7A38,3,0xEEA50E,2,0xCC7A38,1,0x763605,2,0xCC7A38,1,0x975756,1,0x290002,1,0x763605,3,0xF45108,3,0xC3130B,1,0xF45108,3,0xC3130B,3,0xF45108,2,0xC3130B,5,0x763605,2,0xC3130B,1},
    {0xF45108,1,0xC3130B,1,0x763605,1,0x290002,1,0x763605,5,0xCC7A38,1,0xC3130B,1,0xCC7A38,2,0xEEA50E,3,0xFDDA5C,1,0xEEA50E,1,0xCC7A38,1,0x763605,6,0x9A8A0B,1,0xEEA50E,2,0xCC7A38,1,0xF45108,1,0xCC7A38,1,0x763605,1,0xC3130B,1,0x763605,2,0xC3130B,3,0xEEA50E,2,0xCC7A38,1,0x763605,3},
    {0xCC7A38,3,0x763605,1,0x290002,1,0x763605,6,0x975756,1,0x763605,1,0xCC7A38,1,0xEEA50E,3,0xFDDA5C,1,0x975756,1,0x763605,7,0x9A8A0B,1,0xEEA50E,3,0x763605,1,0xCC7A38,1,0x763605,6,0xCC7A38,2,0xC3130B,1,0x763605,3},
    {0xCC7A38,1,0x763605,4,0x290002,1,0x763605,6,0x290002,1,0xC3130B,1,0xCC7A38,2,0xFDDA5C,1,0xCC7A38,1,0x763605,3,0xFDDA5C,2,0xCC7A38,1,0x975756,1,0x763605,2,0xCC7A38,1,0xEEA50E,1,0x975756,1,0xCC7A38,1,0xFDDA5C,1,0xCC7A38,1,0x763605,1,0x290002,1,0x763605,1,0xCC7A38,1,0xC3130B,4,0x763605,1,0x290002,1,0x763605,1},
    {0xEEA50E,1,0xCC7A38,1,0x763605,11,0xCC7A38,2,0xEEA50E,1,0xCC7A38,1,0x763605,3,0xFDDA5C,4,0xCC7A38,1,0xC3130B,2,0xCC7A38,1,0xEEA50E,1,0xC3130B,1,0xCC7A38,1,0xF45108,2,0x763605,4,0xC3130B,1,0xF45108,2,0xC3130B,1,0x290002,2,0x763605,1},
    {0xEEA50E,1,0xCC7A38,1,0x290002,1,0x763605,4,0x290002,3,0x763605,2,0xCC7A38,1,0xEEA50E,1,0xF45108,1,0xEEA50E,1,0xC3130B,3,0xFDDA5C,1,0xCC7A38,2,0xC3130B,1,0xF45108,1,0xC3130B,3,0xFDDA5C,1,0xEEA50E,3,0xCC7A38,1,0xC3130B,1,0x763605,7,0xC3130B,1,0x763605,2,0x290002,1},
    {0xEEA50E,1,0xCC7A38,1,0x290002,1,0x763605,1,0xCC7A38,1,0xF45108,1,0x763605,1,0x290002,1,0x763605,1,0x290002,1,0x763605,4,0xCC7A38,2,0x763605,2,0xCC7A38,2,0xC3130B,1,0xF45108,1,0xC3130B,5,0xCC7A38,1,0xC3130B,2,0xCC7A38,1,0xFDDA5C,1,0xCC7A38,1,0x763605,5,0x290002,3,0xCC7A38,1,0x763605,1,0x290002,1},
    {0xCC7A38,2,0x763605,4,0x290002,3,0x763605,1,0x290002,3,0x763605,1,0xCC7A38,1,0x763605,3,0xC3130B,1,0xF45108,1,0xC3130B,10,0xF45108,2,0xEEA50E,2,0x763605,1,0x290002,6,0x763605,3},
    {0x763605,4,0xCC7A38,1,0x763605,2,0x290002,3,0x763605,1,0xCC7A38,1,0x763605,2,0xC3130B,2,0x763605,2,0xC3130B,2,0x763605,1,0xC3130B,1,0xF45108,1,0xC3130B,10,0xCC7A38,2,0x290002,2,0x763605,2,0x290002,5},
    {0x290002,2,0x763605,6,0x290002,1,0x763605,1,0xC3130B,2,0x763605,2,0xC3130B,1,0xF45108,1,0xC3130B,3,0x763605,1,0x290002,1,0x763605,2,0xF45108,1,0xC3130B,1,0xF45108,1,0xC3130B,4,0xCC7A38,1,0xC3130B,3,0xEEA50E,1,0xCC7A38,1,0x290002,1,0x9A8A0B,1,0xF45108,1,0x763605,1,0x290002,2,0x763605,2},
    {0x763605,1,0x975756,1,0xC3130B,1,0x763605,2,0x290002,1,0x763605,1,0xC3130B,1,0x763605,3,0x290002,3,0xC3130B,1,0xF45108,1,0xC3130B,3,0x763605,1,0x290002,2,0xC3130B,1,0xF45108,1,0xC3130B,2,0xF45108,1,0xC3130B,7,0xF45108,1,0x763605,1,0x290002,1,0x763605,1,0xC3130B,1,0x763605,5},
    {0x763605,4,0x290002,2,0x763605,2,0x9A8A0B,1,0x763605,1,0x290002,2,0x763605,1,0x290002,1,0x763605,11,0xC3130B,4,0xF45108,2,0xC3130B,1,0xF45108,1,0xC3130B,1,0x763605,10},
    {0x290002,1,0x763605,3,0x290002,2,0x763605,4,0x290002,1,0xC3130B,1,0xF45108,1,0xC3130B,1,0x290002,5,0x763605,1,0x290002,2,0x763605,1,0x290002,1,0x763605,1,0xC3130B,2,0xF45108,2,0x9A8A0B,1,0xF45108,2,0x763605,5,0x975756,1,0x763605,6},
    {0x763605,4,0x290002,6,0x763605,1,0xC3130B,2,0x763605,1,0x290002,1,0xC3130B,2,0x290002,2,0x763605,1,0x290002,2,0x763605,4,0xC3130B,2,0xF45108,1,0x763605,1,0xC3130B,2,0x763605,11,0x290002,1},
    {0x763605,2,0x290002,2,0x763605,6,0x290002,1,0xC3130B,1,0x763605,1,0x290002,1,0x763605,1,0xC3130B,1,0x763605,1,0x290002,6,0xC3130B,2,0x290002,1,0x763605,2,0xC3130B,1,0x290002,1,0x763605,1,0xF45108,1,0xC3130B,1,0x763605,9,0x290002,2},
    {0x290002,1,0x763605,7,0xC3130B,1,0x763605,3,0xC3130B,1,0x290002,1,0x763605,1,0xC3130B,1,0x763605,1,0x290002,5,0x763605,1,0xC3130B,2,0x763605,1,0x290002,1,0x763605,1,0xC3130B,1,0x763605,1,0x290002,1,0x763605,1,0xC3130B,1,0x763605,1,0x290002,2,0x763605,2,0x290002,6},
    {0x290002,1,0x763605,2,0x290002,3,0x763605,1,0xC3130B,1,0x763605,3,0xC3130B,1,0x763605,1,0x290002,2,0x763605,2,0x290002,5,0xC3130B,4,0x763605,1,0x290002,1,0x763605,2,0x290002,2,0x763605,2,0x290002,10},
    {0x290002,8,0x763605,9,0x290002,5,0x763605,6,0x290002,1,0x763605,1,0x290002,14},
}

-- Иконки рабочего стола.
--   win_title  → клик открывает / фокусирует встроенное Lua-окно.
--   exec       → клик запускает внешний ELF через системный вызов.
-- Эти два поля взаимоисключающи; обработчик кликов смотрит сначала на exec.
local desktop_icons = {
    { label = "Terminal", icon_col = 0x1E1E24, text = ">_", win_title = "Equinox Terminal" },
    { label = "Monitor",  icon_col = 0x0078D7, text = "M",  win_title = "System Monitor" },
    { label = "Paint",    icon_col = 0xFF6B00, text = "P",  win_title = "Vector Paint Brush" },
    { label = "Explorer", icon_col = 0xF0C040, text = "E",  win_title = "VFS File Explorer" },
    { label = "Notepad",  icon_col = 0x2B579A, text = "N",  win_title = "Notepad Text Editor" },
    { label = "Doom",     icon_col = 0x8B0000, text = "",   exec = "bin/doom.elf -iwad res/doom1.wad", pixels = doom_pixels },
}

-- Инициализируем объекты окон
table.insert(windows, Window.new("Equinox Terminal", 50, 80, 500, 320, draw_terminal))
table.insert(windows, Window.new("System Monitor", 600, 80, 320, 140, draw_monitor))
table.insert(windows, Window.new("Vector Paint Brush", 120, 200, 420, 300, draw_paint))
table.insert(windows, Window.new("VFS File Explorer", 400, 150, 350, 260, draw_explorer))
table.insert(windows, Window.new("Notepad Text Editor", 100, 100, 400, 260, draw_notepad))

-- Все окна изначально активны
for _, win in ipairs(windows) do win.active = true end
focused_window = windows[1] -- Фокус по умолчанию на Терминале

-- Сканируем файловую систему при старте
refresh_explorer()

-- --- ГЛАВНЫЙ ИГРОВОЙ ЦИКЛ GUI (ON_TICK) ---
local dragging_win = nil
local drag_ox, drag_oy = 0, 0

local function bring_to_front(win)
    for i, w in ipairs(windows) do
        if w == win then
            table.remove(windows, i)
            table.insert(windows, win)
            break
        end
    end
end

function on_tick(dt)
    local sw, sh = getScreenSize() 
    local mx, my, mdown = getMouse()

    -- 1. Рендеринг обоев
    local task_y = sh - 32
    drawGradient(0, 0, sw, task_y, 0x101216, 0x1E222D, true)

    -- 2. Обработка Resize
    if resizing_win then
        if mdown then
            resizing_win.w = mx - resizing_win.x
            resizing_win.h = my - resizing_win.y
            if resizing_win.w < 100 then resizing_win.w = 100 end
            if resizing_win.h < 60 then resizing_win.h = 60 end
            _G.needs_redraw = true
        else
            resizing_win = nil
        end
    end

    -- 3. Обработка ввода (Клавиатура)
    local key = getLastKey()
    if key > 0 then
        if key == 42 or key == 54 then shift_pressed = true end
        local char = scancodeToAscii(key, shift_pressed)

        if focused_window then
            if focused_window.title == "Equinox Terminal" then
                if key == 28 then 
                    table.insert(term_lines, ">> " .. term_input)
                    process_command(term_input)
                    term_input = ""
                elseif key == 14 then 
                    term_input = string.sub(term_input, 1, -2)
                elseif string.len(char) > 0 and string.byte(char) >= 32 then
                    term_input = term_input .. char
                end
            elseif focused_window.title == "Notepad Text Editor" then
                if key == 28 then 
                    _G.notepad_text = _G.notepad_text .. "\n"
                elseif key == 14 then 
                    _G.notepad_text = string.sub(_G.notepad_text, 1, -2)
                elseif string.len(char) > 0 and string.byte(char) >= 32 then
                    _G.notepad_text = _G.notepad_text .. char
                end
            end
        end
    else
        shift_pressed = false
    end

    -- 4. Отрисовка иконок рабочего стола
    for i, icon in ipairs(desktop_icons) do
        local ix, iy = 20, 30 + (i - 1) * 90
        drawRect(ix, iy, 48, 48, 0x14151B)
        
        if icon.pixels then
            for row_idx = 1, 44 do
                local row = icon.pixels[row_idx]
                if row then
                    local px, py, k = ix + 2, iy + 1 + row_idx, 1
                    while row[k] do
                        drawRect(px, py, row[k+1], 1, row[k])
                        px = px + row[k+1]
                        k = k + 2
                    end
                end
            end
        else
            drawRect(ix + 2, iy + 2, 44, 44, icon.icon_col)
            if icon.text then drawText(icon.text, ix + 18, iy + 16, 0xFFFFFF) end
        end
        drawText(icon.label, ix + 2, iy + 52, 0xD8DEE9)

        -- Клик по иконке (запуск приложений)
        if not dragging_win and not resizing_win and mdown and not last_mdown then
            if mx >= ix and mx < ix + 48 and my >= iy and my < iy + 48 then
                if icon.exec then
                    exec(icon.exec)
                    -- Если это Doom, активируем контейнер
                    if icon.label == "Doom" then
                        for _, w in ipairs(windows) do
                            if w.is_app_container then
                                w.active = true
                                focused_window = w
                                bring_to_front(w)
                            end
                        end
                    end
                elseif icon.win_title then
                    for _, w in ipairs(windows) do
                        if w.title == icon.win_title then
                            w.active = true
                            focused_window = w
                            bring_to_front(w)
                        end
                    end
                end
            end
        end
    end

    -- 5. Перемещение окон и Фокус (только если не ресайзим и не кликаем иконки)
    if mdown and not last_mdown and not resizing_win then
        local found = false
        for i = #windows, 1, -1 do
            local win = windows[i]
            if win.active then
                -- Попали в заголовок или тело?
                if mx >= win.x and mx < win.x + win.w and my >= win.y - 28 and my < win.y + win.h then
                    focused_window = win
                    bring_to_front(win)
                    
                    if my < win.y then -- Клик по заголовку
                        dragging_win = win
                        drag_ox, drag_oy = mx - win.x, my - win.y
                    end
                    found = true
                    break
                end
            end
        end
        if not found and mx > 80 then focused_window = nil end -- Сброс фокуса если кликнули мимо (но не по иконкам)
    end

    if not mdown then dragging_win = nil end

    if dragging_win then
        dragging_win.x = mx - drag_ox
        dragging_win.y = my - drag_oy
        if dragging_win.y < 28 then dragging_win.y = 28 end
        _G.needs_redraw = true
    end

    -- 6. Отрисовка Окон
    for _, win in ipairs(windows) do
        win:draw(mx, my, mdown, dt)
    end

    -- 7. Нижняя панель (Taskbar)
    drawGradient(0, task_y, sw, 32, 0x1B1F2A, 0x12151D, true)
    drawRect(0, task_y, sw, 1, 0x2A2E3D)

    local tx = 15
    for _, win in ipairs(windows) do
        if win.active and not win.borderless then
            local active = (focused_window == win)
            drawRect(tx, task_y + 4, 120, 24, active and 0x0078D7 or 0x2C313C)
            drawText(string.sub(win.title, 1, 12), tx + 10, task_y + 8, 0xE5E9F0)
            
            if mdown and not last_mdown and mx >= tx and mx < tx + 120 and my >= task_y + 4 then
                focused_window = win
                bring_to_front(win)
            end
            tx = tx + 125
        end
    end

    -- Статистика в углу
    local used, total = getMemInfo()
    local used_mb = math.floor(used / (1024 * 1024))
    drawText(string.format("RAM: %d MB", used_mb), sw - 200, task_y + 10, 0x888C94)
    
    local up = getUptime()
    local h = math.floor(up / 3600)
    local m = math.floor((up / 60) % 60)
    local s = math.floor(up % 60)
    drawText(string.format("%02d:%02d:%02d", h, m, s), sw - 100, task_y + 10, 0xD8DEE9)

    last_mdown = mdown
end

-- Создаем спец-окно для Doom/Snake
local app_container = Window.new("External Application", 250, 150, 640, 400, nil)
app_container.is_app_container = true
app_container.active = false
table.insert(windows, app_container)