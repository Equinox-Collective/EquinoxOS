-- Equinox Desktop Environment (enGUI)
print("enGUI Desktop Environment: Windows & Apps loaded!")

local windows = {}
local focused_window = nil
local last_mdown = false

-- Флаг грязного кадра: Lua выставляет в true, если нужна перерисовка
-- (matrix mode, мигающий курсор). C читает через getLastKey() side-effect.
_G.needs_redraw = false

-- Состояние глобального ввода для Блокнота / Терминала
local shift_pressed = false
-- Кэш последнего значения blink для отслеживания переходов
local last_blink_state = -1

-- --- КЛАСС WINDOW (ООП МЕНЕДЖЕР ОКОН) ---
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
    self.draw_cb = draw_cb
    return self
end

function Window:draw(mx, my, mdown, dt)
    if not self.active then return end

    local active = (focused_window == self)
    local ty = self.y - 25

    -- 1. Рамка и тень (обводка)
    drawRect(self.x - 1, ty - 1, self.w + 2, self.h + 27, 0x111216)

    -- 2. Градиентный заголовок
    if active then
        drawGradient(self.x, ty, self.w, 25, 0x0078D7, 0x005A9E, true)
    else
        drawGradient(self.x, ty, self.w, 25, 0x44464F, 0x2E3037, true)
    end

    -- 3. Текст заголовка
    drawText(self.title, self.x + 8, ty + 5, 0xFFFFFF)

    -- 4. Кнопка закрытия [X] (Стиль Windows 10)
    local bx = self.x + self.w - 24
    local by = ty + 4
    local bw, bh = 18, 16
    local over_close = (mx >= bx and mx < bx + bw and my >= by and my < by + bh)
    
    if over_close then
        drawRect(bx, by, bw, bh, 0xE81123)
    else
        drawRect(bx, by, bw, bh, 0xCC2A2A)
    end
    drawText("X", bx + 5, by + 1, 0xFFFFFF)

    if over_close and mdown and not last_mdown then
        self.active = false
        if focused_window == self then focused_window = nil end
        return
    end

    -- 5. Фон рабочей области окна
    drawRect(self.x, self.y, self.w, self.h, 0x1E1F29)

    -- 6. Отрисовка внутреннего содержимого (Callback)
    if self.draw_cb then
        self.draw_cb(self, mx, my, mdown, dt)
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

local function process_command(cmd)
    cmd = string.gsub(cmd, "%s+", "") -- убираем лишние пробелы
    if cmd == "help" then
        table.insert(term_lines, "Commands: help, clear, matrix, doom, snake")
    elseif cmd == "clear" then
        term_lines = {}
    elseif cmd == "matrix" then
        matrix_mode = not matrix_mode
        table.insert(term_lines, "Matrix digital rain: " .. (matrix_mode and "ENABLED" or "DISABLED"))
    elseif cmd == "doom" then
        table.insert(term_lines, "Launching doom.elf...")
        exec("bin/doom.elf -iwad res/doom1.wad")
    elseif cmd == "snake" then
        table.insert(term_lines, "Launching snake.elf...")
        exec("bin/snake.elf")
    elseif cmd ~= "" then
        table.insert(term_lines, "Unknown command: '" .. cmd .. "'")
    end
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

    -- Обработка рисования (холст ниже панели инструментов)
    local canvas_y = win.y + 25
    local inside_canvas = (mx >= win.x and mx < win.x + win.w and my >= canvas_y and my < win.y + win.h)

    if inside_canvas and mdown then
        if not active_stroke then
            active_stroke = { color = active_color, points = {} }
            table.insert(paint_strokes, active_stroke)
        end
        table.insert(active_stroke.points, { x = mx, y = my })
    else
        active_stroke = nil
    end

    -- Рендеринг всех нарисованных мазков
    for _, stroke in ipairs(paint_strokes) do
        local points = stroke.points
        for k = 1, #points - 1 do
            local p1 = points[k]
            local p2 = points[k+1]
            drawLine(p1.x, p1.y, p2.x, p2.y, stroke.color)
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

    for idx, f in ipairs(files_list) do
        local row_y = draw_y + (idx - 1) * 22
        local is_hover = (mx >= win.x and mx < win.x + win.w and my >= row_y and my < row_y + 20)

        -- Эффект выделения строки
        if is_hover then
            drawRect(win.x, row_y, win.w, 20, 0x333644)
        end

        -- Значок диска (EXT2 синий, FAT желтый)
        local icon_col = (f.dev == "EXT2_DISK") and 0x5E81AC or 0xEBCB8B
        drawRect(win.x + 8, row_y + 5, 10, 10, icon_col)

        -- Название и размер
        drawText(f.name, win.x + 26, row_y + 3, 0xE5E9F0)
        drawText(f.dev, win.x + win.w - 120, row_y + 3, 0x888C94)

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
    { label = "Doom",     icon_col = 0x8B0000, text = "",   exec = "bin/doom.elf", pixels = doom_pixels },
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

function on_tick(dt)
    local sw, sh = getScreenSize() 
    local mx, my, mdown = getMouse()
    _G.needs_redraw = false  -- сбрасываем в начале каждого тика

    -- 1. Рендеринг обоев (Красивый плавный космический фон)
    drawGradient(0, 0, sw, sh - 32, 0x101216, 0x1E222D, true)

    -- 2. Обработка ввода (Считывание сканкодов клавиш)
    local key = getLastKey()
    if key > 0 then
        -- Поддержка Shift
        if key == 42 or key == 54 then
            shift_pressed = true
        end

        local char = scancodeToAscii(key, shift_pressed)

        -- Куда направлять ввод?
        if focused_window then
            if focused_window.title == "Equinox Terminal" then
                if key == 28 then -- ENTER
                    table.insert(term_lines, ">> " .. term_input)
                    process_command(term_input)
                    term_input = ""
                elseif key == 14 then -- Backspace
                    term_input = string.sub(term_input, 1, -2)
                elseif string.len(char) > 0 and string.byte(char) >= 32 then
                    term_input = term_input .. char
                end
            elseif focused_window.title == "Notepad Text Editor" then
                if key == 28 then -- ENTER
                    _G.notepad_text = _G.notepad_text .. "\n"
                elseif key == 14 then -- Backspace
                    _G.notepad_text = string.sub(_G.notepad_text, 1, -2)
                elseif string.len(char) > 0 and string.byte(char) >= 32 then
                    _G.notepad_text = _G.notepad_text .. char
                end
            end
        end
    else
        -- Сброс шифта
        shift_pressed = false
    end

    -- 3. Отрисовка рабочего стола (иконок)
    for i, icon in ipairs(desktop_icons) do
        local ix = 20
        local iy = 30 + (i - 1) * 90

        -- Рисуем обводку и тело иконки
        drawRect(ix, iy, 48, 48, 0x14151B)
        if icon.pixels then
            -- Иконка-картинка: 44 строки run-length encoded пикселей
            -- (color, run_length) внутри inner-области 44x44.
            for row_idx = 1, 44 do
                local row = icon.pixels[row_idx]
                if row then
                    local px = ix + 2
                    local py = iy + 1 + row_idx
                    local k = 1
                    while row[k] do
                        local c = row[k]
                        local len = row[k + 1]
                        drawRect(px, py, len, 1, c)
                        px = px + len
                        k = k + 2
                    end
                end
            end
        else
            drawRect(ix + 2, iy + 2, 44, 44, icon.icon_col)
            if icon.text and icon.text ~= "" then
                drawText(icon.text, ix + 18, iy + 16, 0xFFFFFF)
            end
        end
        drawText(icon.label, ix + 2, iy + 52, 0xD8DEE9)

        -- Клик на иконку рабочего стола.
        -- Если у иконки задан exec — запускаем внешний ELF (например, Doom);
        -- иначе ищем по win_title встроенное Lua-окно и фокусируем его.
        if mx >= ix and mx < ix + 48 and my >= iy and my < iy + 64 then
            if mdown and not last_mdown then
                if icon.exec then
                    exec(icon.exec)
                elseif icon.win_title then
                    for _, win in ipairs(windows) do
                        if win.title == icon.win_title then
                            win.active = true
                            focused_window = win
                            -- Выводим окно наверх стопки отрисовки
                            for k, w in ipairs(windows) do
                                if w == win then
                                    table.remove(windows, k)
                                    break
                                end
                            end
                            table.insert(windows, win)
                            break
                        end
                    end
                end
            end
        end
    end

    -- 4. Перемещение окон (Drag & Drop) и смена фокуса
    if mdown and not last_mdown then
        local clicked_win = nil
        -- Ищем окно от верхнего слоя к нижнему
        for i = #windows, 1, -1 do
            local win = windows[i]
            if win.active then
                -- Проверка попадания в заголовок (для перемещения)
                if mx >= win.x and mx < win.x + win.w and my >= win.y - 25 and my < win.y then
                    clicked_win = win
                    break
                -- Проверка попадания в тело (для смены фокуса)
                elseif mx >= win.x and mx < win.x + win.w and my >= win.y and my < win.y + win.h then
                    focused_window = win
                    -- Перемещаем окно в самый конец списка отрисовки
                    table.remove(windows, i)
                    table.insert(windows, win)
                    break
                end
            end
        end

        if clicked_win then
            focused_window = clicked_win
            dragging_win = clicked_win
            drag_ox = mx - clicked_win.x
            drag_oy = my - clicked_win.y

            -- Перемещаем наверх стопки
            for k, w in ipairs(windows) do
                if w == clicked_win then
                    table.remove(windows, k)
                    break
                end
            end
            table.insert(windows, clicked_win)
        end
    end

    if not mdown then
        dragging_win = nil
    end

    if dragging_win then
        dragging_win.x = mx - drag_ox
        dragging_win.y = my - drag_oy
        -- Ограничение: заголовок не может уползти за верхнюю границу экрана
        if dragging_win.y < 25 then dragging_win.y = 25 end
    end

    -- 5. Рендеринг всех активных окон (в порядке слоев)
    for _, win in ipairs(windows) do
        win:draw(mx, my, mdown, dt)
    end

    -- 6. Нижняя системная панель задач (Taskbar)
    local task_y = 736
    drawGradient(0, task_y, sw, sh - 32, 0x1B1F2A, 0x12151D, true)
    drawRect(0, task_y, sw, 1, 0x2A2E3D)

    -- Быстрый список открытых окон в левой части
    local tx_start = 15
    for _, win in ipairs(windows) do
        if win.active then
            local active = (focused_window == win)
            local tag_col = active and 0x0078D7 or 0x2C313C
            drawRect(tx_start, task_y + 4, 120, 24, tag_col)
            drawText(string.sub(win.title, 1, 12), tx_start + 10, task_y + 8, 0xE5E9F0)

            -- Клик по панели задач фокусирует окно
            if mx >= tx_start and mx < tx_start + 120 and my >= task_y + 4 and my < task_y + 28 then
                if mdown and not last_mdown then
                    focused_window = win
                    for k, w in ipairs(windows) do
                        if w == win then
                            table.remove(windows, k)
                            break
                        end
                    end
                    table.insert(windows, win)
                end
            end
            tx_start = tx_start + 128
        end
    end

    -- Статус RAM и Uptime в правой части панели задач
    local used, total = getMemInfo()
    local used_mb = math.floor(used / (1024 * 1024))
    local ram_str = string.format("RAM: %d MB", used_mb)
    drawText(ram_str, sw - 200, task_y + 10, 0x888C94)

    local uptime = getUptime()
    local s = math.floor(uptime % 60)
    local m = math.floor((uptime / 60) % 60)
    local h = math.floor(uptime / 3600)
    local clock_str = string.format("%02d:%02d:%02d", h, m, s)
    drawText(clock_str, sw - 100, task_y + 10, 0xD8DEE9)

    last_mdown = mdown
end