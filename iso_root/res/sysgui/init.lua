-- res/sysgui/init.lua
-- Equinox Desktop Environment (enGUI) - Modular Edition
print("enGUI Desktop Environment: Loading modules...")

-- Глобальные состояния менеджера окон
windows = {}
focused_window = nil
last_mdown = false
resizing_win = nil

-- Флаг грязного кадра (отслеживается C-слоем)
_G.needs_redraw = false

local shift_pressed = false

-- Инициализируем модули
local Window = dofile("res/sysgui/window.lua")
local draw_terminal = dofile("res/sysgui/terminal.lua")
local draw_monitor = dofile("res/sysgui/monitor.lua")
local draw_paint = dofile("res/sysgui/paint.lua")
local draw_explorer = dofile("res/sysgui/explorer.lua")
local draw_notepad = dofile("res/sysgui/notepad.lua")

-- RLE-иконка Doom 44x44
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

-- Конфигурация ярлыков рабочего стола
local desktop_icons = {
    { label = "Terminal", icon_col = 0x1E1E24, text = ">_", win_title = "Equinox Terminal" },
    { label = "Monitor",  icon_col = 0x0078D7, text = "M",  win_title = "System Monitor" },
    { label = "Paint",    icon_col = 0xFF6B00, text = "P",  win_title = "Vector Paint Brush" },
    { label = "Explorer", icon_col = 0xF0C040, text = "E",  win_title = "VFS File Explorer" },
    { label = "Notepad",  icon_col = 0x2B579A, text = "N",  win_title = "Notepad Text Editor" },
    { label = "Doom",     icon_col = 0x8B0000, text = "",   exec = "bin/doom.elf -iwad res/doom1.wad", pixels = doom_pixels },
}

-- Инициализируем системные окна через подключенные модули
table.insert(windows, Window.new("Equinox Terminal", 50, 80, 500, 320, draw_terminal))
table.insert(windows, Window.new("System Monitor", 600, 80, 320, 140, draw_monitor))
table.insert(windows, Window.new("Vector Paint Brush", 120, 200, 420, 300, draw_paint))
table.insert(windows, Window.new("VFS File Explorer", 400, 150, 350, 260, draw_explorer))
table.insert(windows, Window.new("Notepad Text Editor", 100, 100, 400, 260, draw_notepad))

-- Все окна изначально развернуты
for _, win in ipairs(windows) do win.active = true end
focused_window = windows[1] -- Фокус по умолчанию на Терминале

-- Инициализируем список файлов
if type(_G.refresh_explorer) == "function" then
    _G.refresh_explorer()
end

-- Перемещение окон по оси Z
local function bring_to_front(win)
    for i, w in ipairs(windows) do
        if w == win then
            table.remove(windows, i)
            table.insert(windows, win)
            break
        end
    end
end

-- --- ГЛАВНЫЙ ЦИКЛ ОБРАТНОГО ВЫЗОВА GUI ---
local dragging_win = nil
local drag_ox, drag_oy = 0, 0

function on_tick(dt)
    local sw, sh = getScreenSize() 
    local mx, my, mdown = getMouse()

    -- 1. Отрисовка обоев
    local task_y = sh - 32
    drawGradient(0, 0, sw, task_y, 0x101216, 0x1E222D, true)

    -- 2. Обработка изменения размера
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
                    table.insert(_G.term_lines, ">> " .. _G.term_input)
                    if type(_G.process_terminal_command) == "function" then
                        _G.process_terminal_command(_G.term_input)
                    end
                    _G.term_input = ""
                elseif key == 14 then 
                    _G.term_input = string.sub(_G.term_input, 1, -2)
                elseif string.len(char) > 0 and string.byte(char) >= 32 then
                    _G.term_input = _G.term_input .. char
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

        -- Обработка клика по иконкам (запуск приложений)
        if not dragging_win and not resizing_win and mdown and not last_mdown then
            if mx >= ix and mx < ix + 48 and my >= iy and my < iy + 48 then
                if icon.exec then
                    exec(icon.exec)
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

    -- 5. Перемещение окон и Фокус
    if mdown and not last_mdown and not resizing_win then
        local found = false
        for i = #windows, 1, -1 do
            local win = windows[i]
            if win.active then
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
        if not found and mx > 80 then focused_window = nil end
    end

    if not mdown then dragging_win = nil end

    if dragging_win then
        dragging_win.x = mx - drag_ox
        dragging_win.y = my - drag_oy
        if dragging_win.y < 28 then dragging_win.y = 28 end
        _G.needs_redraw = true
    end

    -- 6. Отрисовка всех окон
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

    -- Статистика в углу панели задач
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

-- Создаем спец-контейнер для полноэкранных ELF приложений (Doom/Snake)
local app_container = Window.new("External Application", 250, 150, 640, 400, nil)
app_container.is_app_container = true
app_container.active = false
table.insert(windows, app_container)