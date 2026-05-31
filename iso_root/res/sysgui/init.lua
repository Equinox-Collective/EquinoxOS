print("enGUI Desktop Environment: Loading modular core...")

windows = {}
focused_window = nil
last_mdown = false
resizing_win = nil

_G.needs_redraw = false
_G.shift_pressed = false
_G.ctrl_pressed = false
_G.alt_pressed = false

local Window = dofile("res/sysgui/window.lua")
local draw_terminal = dofile("res/sysgui/terminal.lua")
local draw_monitor = dofile("res/sysgui/monitor.lua")
local draw_paint = dofile("res/sysgui/paint.lua")
local draw_explorer = dofile("res/sysgui/explorer.lua")
local draw_notepad = dofile("res/sysgui/notepad.lua")

local app_container = Window.new("External Application", 250, 150, 640, 400, nil)
app_container.is_app_container = true
app_container.active = false

-- Оборонительный запуск интро через pcall [3]
local system_state = "BOOT"
local bootvid = nil

-- Теперь результат dofile запишется во вторую переменную (bootvid)
local bootvid_ok, res = pcall(dofile, "res/sysgui/bootvid.lua")

if bootvid_ok and type(res) == "table" and type(res.init) == "function" then
    bootvid = res
    bootvid.init()
else
    print("enGUI Warning: bootvid.lua missing or failed to load. Skipping intro. Error: " .. tostring(res))
    system_state = "DESKTOP" -- Мгновенный откат на рабочий стол, если файл потерялся
end

-- Screensaver States
local start_menu_open = false
local last_input_time = getUptime()
local screensaver_active = false
local stars = {}

local function init_screensaver()
    for i = 1, 60 do
        stars[i] = {
            x = math.random(-300, 300),
            y = math.random(-300, 300),
            z = math.random(1, 400)
        }
    end
end
init_screensaver()

-- Theme Definition
local themes = {
    { c1 = 0x0A0B10, c2 = 0x1A1C24, name = "Cosmos Blue" },
    { c1 = 0x0B0606, c2 = 0x1F0F0F, name = "Dracula Velvet" },
    { c1 = 0x050808, c2 = 0x111C1C, name = "Cyberpunk Teal" }
}
local current_theme_idx = 1

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
    {0x290002,2,0xC3130B,1,0x763605,2,0xC3130B,2,0xEACBC7,1,0xFFFFFF,1,0xEACBC7,1,0xC3130B,3,0x763605,1,0xC3130B,1,0x763605,6,0xC3130B,13,0xEACBC7,1,0xFFFFFF,1,0x975756,1,0xC3130B,3,0x763605,2,0xC3130B,1,0x763605,1},
    {0x290002,1,0x763605,6,0xBA9B9A,2,0xC3130B,5,0x763605,1,0xC3130B,1,0x763605,1,0xC3130B,2,0x763605,2,0xC3130B,14,0xEACBC7,1,0x975756,1,0xC3130B,5,0x763605,2},
    {0xC3130B,1,0x763605,1,0x290002,1,0x763605,2,0xC3130B,8,0x763605,1,0xC3130B,3,0x763605,1,0xC3130B,2,0x763605,1,0xC3130B,2,0x763605,3,0xC3130B,18},
    {0xC3130B,2,0x763605,3,0xC3130B,1,0x290002,1,0xC3130B,9,0x763605,1,0xC3130B,3,0x763605,1,0xC3130B,2,0x763605,6,0xC3130B,15},
    {0xC3130B,1,0x290002,1,0xC3130B,1,0x763605,1,0xC3130B,2,0x290002,1,0xC3130B,6,0x290002,1,0xC3130B,7,0x763605,1,0xC3130B,2,0x763605,8,0xC3130B,4,0x763605,3,0xC3130B,5},
    {0xC3130B,2,0x290002,1,0x763605,1,0xC3130B,2,0x290002,1,0xC3130B,2,0x763605,1,0xC3130B,2,0x290002,4,0xC3130B,2,0x290002,3,0xC3130B,3,0x763605,12,0xC3130B,7,0x763605,1},
    {0xC3130B,2,0xCC7A38,3,0x290002,1,0xC3130B,1,0x763605,2,0xC3130B,1,0x290002,10,0xCC7A38,2,0x290002,1,0xCC7A38,1,0x763605,4,0xC3130B,3,0x763605,5,0xC3130B,6,0x763605,2},
    {0xC3130B,2,0x763605,1,0xC3130B,2,0xCC7A38,1,0x763605,1,0x975756,1,0xCC7A38,1,0x290002,3,0xEEA50E,1,0x290002,1,0xC3130B,1,0x290002,3,0xEEA50E,1,0xCC7A38,1,0x763605,3,0x9A8A0B,2,0x763605,3,0xC3130B,3,0x975756,1,0x763605,1,0xCC7A38,2,0x763605,2,0xC3130B,1,0x290002,1,0xC3130B,3,0x763605,2},
    {0xC3130B,2,0x763605,4,0xC3130B,3,0xCC7A38,4,0x290002,1,0xC3130B,2,0x290002,2,0xEEA50E,1,0x9A8A0B,4,0x763605,2,0xC3130B,5,0xCC7A38,1,0xEEA50E,1,0x763605,3,0xC3130B,2,0xCC7A38,2,0xC3130B,2,0x763605,3},
    {0xC3130B,3,0x763605,2,0xC3130B,2,0x763605,1,0xC3130B,1,0x290002,1,0xFDDA5C,1,0xCC7A38,1,0xC3130B,2,0x290002,3,0xCC7A38,1,0x763605,2,0x9A8A0B,3,0x763605,1,0x290002,2,0xC3130B,2,0x290002,1,0xEEA50E,1,0xC3130B,1,0xCC7A38,3,0x290002,1,0xC3130B,2,0xCC7A38,2,0xC3130B,2,0x763605,3},
    {0xC3130B,1,0xCC7A38,1,0x763605,7,0xC3130B,1,0x290002,4,0x763605,1,0xCC7A38,1,0xEEA50E,1,0x9A8A0B,1,0x763605,6,0xCC7A38,1,0x290002,2,0xEEA50E,2,0xC3130B,2,0xCC7A38,2,0x290002,1,0xCC7A38,1,0x290002,1,0xCC7A38,2,0xC3130B,2,0x763605,2,0xC3130B,2},
    {0xC3130B,2,0x763605,3,0xC3130B,1,0x763605,4,0xCC7A38,1,0xC3130B,1,0x290002,1,0xEEA50E,1,0x290002,1,0x763605,2,0x9A8A0B,1,0x763605,6,0x290002,4,0xEEA50E,3,0xCC7A38,1,0x290002,4,0xC3130B,4,0x763605,3,0xC3130B,1},
    {0xC3130B,2,0x763605,1,0x290002,1,0x763605,6,0xCC7A38,3,0xEEA50E,2,0xCC7A38,1,0x763605,2,0xCC7A38,1,0x975756,1,0x290002,1,0x763605,3,0x290002,3,0xC3130B,1,0x290002,3,0xC3130B,3,0x290002,2,0xC3130B,5,0x763605,2,0xC3130B,1},
    {0x290002,1,0xC3130B,1,0x763605,1,0x290002,1,0x763605,5,0xCC7A38,1,0xC3130B,1,0xCC7A38,2,0xEEA50E,3,0xFDDA5C,1,0xEEA50E,1,0xCC7A38,1,0x763605,6,0x9A8A0B,1,0xEEA50E,2,0xCC7A38,1,0x290002,1,0xCC7A38,1,0x763605,1,0xC3130B,1,0x763605,2,0xC3130B,3,0xEEA50E,2,0xCC7A38,1,0x763605,3},
    {0xCC7A38,3,0x763605,1,0x290002,1,0x763605,6,0x975756,1,0x763605,1,0xCC7A38,1,0xEEA50E,3,0xFDDA5C,1,0x975756,1,0x763605,7,0x9A8A0B,1,0xEEA50E,3,0x763605,1,0xCC7A38,1,0x763605,6,0xCC7A38,2,0xC3130B,1,0x763605,3},
    {0xCC7A38,1,0x763605,4,0x290002,1,0x763605,6,0x290002,1,0xC3130B,1,0xCC7A38,2,0xFDDA5C,1,0xCC7A38,1,0x763605,3,0xFDDA5C,2,0xCC7A38,1,0x975756,1,0x763605,2,0xCC7A38,1,0xEEA50E,1,0x975756,1,0xCC7A38,1,0xFDDA5C,1,0xCC7A38,1,0x763605,1,0x290002,1,0x763605,1,0xCC7A38,1,0xC3130B,4,0x763605,1,0x290002,1,0x763605,1},
    {0xEEA50E,1,0xCC7A38,1,0x763605,11,0xCC7A38,2,0xEEA50E,1,0xCC7A38,1,0x763605,3,0xFDDA5C,4,0xCC7A38,1,0xC3130B,2,0xCC7A38,1,0xEEA50E,1,0xC3130B,1,0xCC7A38,1,0x290002,2,0x763605,4,0xC3130B,1,0x290002,2,0xC3130B,1,0x290002,2,0x763605,1},
    {0xEEA50E,1,0xCC7A38,1,0x290002,1,0x763605,4,0x290002,3,0x763605,2,0xCC7A38,1,0xEEA50E,1,0x290002,1,0xEEA50E,1,0xC3130B,3,0xFDDA5C,1,0xCC7A38,2,0xC3130B,1,0x290002,1,0xC3130B,3,0xFDDA5C,1,0xEEA50E,3,0xCC7A38,1,0xC3130B,1,0x763605,7,0xC3130B,1,0x763605,2,0x290002,1},
    {0xEEA50E,1,0xCC7A38,1,0x290002,1,0x763605,1,0xCC7A38,1,0x290002,1,0x763605,1,0x290002,1,0x763605,1,0x290002,1,0x763605,4,0xCC7A38,2,0x763605,2,0xCC7A38,2,0xC3130B,1,0x290002,1,0xC3130B,5,0xCC7A38,1,0xC3130B,2,0xCC7A38,1,0xFDDA5C,1,0xCC7A38,1,0x763605,5,0x290002,3,0xCC7A38,1,0x763605,1,0x290002,1},
    {0xCC7A38,2,0x763605,4,0x290002,3,0x763605,1,0x290002,3,0x763605,1,0xCC7A38,1,0x763605,3,0xC3130B,1,0x290002,1,0xC3130B,10,0x290002,2,0xEEA50E,2,0x763605,1,0x290002,6,0x763605,3},
    {0x763605,4,0xCC7A38,1,0x763605,2,0x290002,3,0x763605,1,0xCC7A38,1,0x763605,2,0xC3130B,2,0x763605,2,0xC3130B,2,0x763605,1,0xC3130B,1,0x290002,1,0xC3130B,10,0xCC7A38,2,0x290002,2,0x763605,2,0x290002,5},
    {0x290002,2,0x763605,6,0x290002,1,0x763605,1,0xC3130B,2,0x763605,2,0xC3130B,1,0x290002,1,0xC3130B,3,0x763605,1,0x290002,1,0x763605,2,0x290002,1,0xC3130B,1,0x290002,1,0xC3130B,4,0xCC7A38,1,0xC3130B,3,0xEEA50E,1,0xCC7A38,1,0x290002,1,0x9A8A0B,1,0x290002,1,0x763605,1,0x290002,2,0x763605,2},
    {0x763605,1,0x975756,1,0xC3130B,1,0x763605,2,0x290002,1,0x763605,1,0xC3130B,1,0x763605,3,0x290002,3,0xC3130B,1,0x290002,1,0xC3130B,3,0x763605,1,0x290002,2,0xC3130B,1,0x290002,1,0xC3130B,2,0x290002,1,0xC3130B,7,0x290002,1,0x763605,1,0x290002,1,0x763605,1,0xC3130B,1,0x763605,5},
    {0x763605,4,0x290002,2,0x763605,2,0x9A8A0B,1,0x763605,1,0x290002,2,0x763605,1,0x290002,1,0x763605,11,0xC3130B,4,0x290002,2,0xC3130B,1,0x290002,1,0xC3130B,1,0x763605,10},
    {0x290002,1,0x763605,3,0x290002,2,0x763605,4,0x290002,1,0xC3130B,1,0x290002,1,0xC3130B,1,0x290002,5,0x763605,1,0x290002,2,0x763605,1,0x290002,1,0x763605,1,0xC3130B,2,0x290002,2,0x9A8A0B,1,0x290002,2,0x763605,5,0x975756,1,0x763605,6},
    {0x763605,4,0x290002,6,0x763605,1,0xC3130B,2,0x763605,1,0x290002,1,0xC3130B,2,0x290002,2,0x763605,1,0x290002,2,0x763605,4,0xC3130B,2,0x290002,1,0x763605,1,0xC3130B,2,0x763605,11,0x290002,1},
    {0x763605,2,0x290002,2,0x763605,6,0x290002,1,0xC3130B,1,0x763605,1,0x290002,1,0x763605,1,0xC3130B,1,0x763605,1,0x290002,6,0xC3130B,2,0x290002,1,0x763605,2,0xC3130B,1,0x290002,1,0x763605,1,0x290002,1,0xC3130B,1,0x763605,9,0x290002,2},
    {0x290002,1,0x763605,7,0xC3130B,1,0x763605,3,0xC3130B,1,0x290002,1,0x763605,1,0xC3130B,1,0x763605,1,0x290002,5,0x763605,1,0xC3130B,2,0x763605,1,0x290002,1,0x763605,1,0xC3130B,1,0x763605,1,0x290002,1,0x763605,1,0xC3130B,1,0x763605,1,0x290002,2,0x763605,2,0x290002,6},
    {0x290002,1,0x763605,2,0x290002,3,0x763605,1,0xC3130B,1,0x763605,3,0xC3130B,1,0x763605,1,0x290002,2,0x763605,2,0x290002,5,0xC3130B,4,0x763605,1,0x290002,1,0x763605,2,0x290002,2,0x763605,2,0x290002,10},
    {0x290002,8,0x763605,9,0x290002,5,0x763605,6,0x290002,1,0x763605,1,0x290002,14},
}

local desktop_icons = {
    { label = "Terminal", icon_col = 0x21252B, text = ">_", win_title = "Equinox Terminal" },
    { label = "Monitor",  icon_col = 0x4B5263, text = "M",  win_title = "System Monitor" },
    { label = "Paint",    icon_col = 0xFF8700, text = "P",  win_title = "Vector Paint Brush" },
    { label = "Explorer", icon_col = 0xE5C07B, text = "E",  win_title = "VFS File Explorer" },
    { label = "Notepad",  icon_col = 0x61AFEF, text = "N",  win_title = "Notepad Text Editor" },
    { label = "Doom",     icon_col = 0xE06C75, text = "",   exec = "bin/doom.elf -iwad res/doom1.wad", pixels = doom_pixels },
}

table.insert(windows, Window.new("Equinox Terminal", 50, 80, 520, 340, draw_terminal.draw, draw_terminal.handle_key))
table.insert(windows, Window.new("System Monitor", 620, 80, 340, 220, draw_monitor))
table.insert(windows, Window.new("Vector Paint Brush", 120, 200, 440, 320, draw_paint))
table.insert(windows, Window.new("VFS File Explorer", 400, 150, 360, 280, draw_explorer))
table.insert(windows, Window.new("Notepad Text Editor", 100, 100, 420, 280, draw_notepad, draw_notepad.handle_key))

-- Все окна остаются неактивными при запуске
for _, win in ipairs(windows) do win.active = false end
focused_window = nil

if type(_G.refresh_explorer) == "function" then
    _G.refresh_explorer()
end

local function bring_to_front(win)
    for i, w in ipairs(windows) do
        if w == win then
            table.remove(windows, i)
            table.insert(windows, win)
            break
        end
    end
end

local dragging_win = nil
local drag_ox, drag_oy = 0, 0

function on_tick(dt)
    -- --- 0. КОНЕЧНЫЙ АВТОМАТ: ЗАГРУЗОЧНАЯ АНИМАЦИЯ ---
    if system_state == "BOOT" and bootvid then
        _G.needs_redraw = true -- <=== СИЛА: Форсируем перерисовку каждого кадра во время загрузки!
        local is_finished = bootvid.draw(dt)
        if is_finished then
            system_state = "DESKTOP"
            _G.needs_redraw = true -- Обновим экран при переходе на рабочий стол
        end
        return
    end

    local sw, sh = getScreenSize() 
    local mx, my, mdown = getMouse()

    -- 1. Inactivity & Screensaver Check
    local is_app_running = false
    for _, w in ipairs(windows) do
        if w.is_app_container and w.active and not w.minimized then
            is_app_running = true
        end
    end

    if app_container.active then
        local tasks = getTasks()
        local external_proc_alive = false
        for _, t in ipairs(tasks) do
            -- Если есть процесс с PID отличным от ядра (1) и sysgui (2)
            if t.pid ~= 1 and t.pid ~= 2 then
                external_proc_alive = true
            end
        end
        -- Если процесс умер, мягко закрываем окно контейнера
        if not external_proc_alive then
            app_container.active = false
            focused_window = nil
            _G.needs_redraw = true 
        end
    end

    -- Если есть активность ИЛИ запущено оконное приложение, сбрасываем таймер
    if mx ~= last_mx or my ~= last_my or mdown ~= last_mdown or getLastKey() > 0 or is_app_running then
        last_input_time = getUptime()
        if screensaver_active then
            screensaver_active = false
            _G.needs_redraw = true -- Использовать этот флаг вместо невидимой Lua функции!
        end
    end

    if getUptime() - last_input_time > 15 then
        screensaver_active = true
    end

    -- Rendering screensaver
    if screensaver_active then
        drawRect(0, 0, sw, sh, 0x000000)
        for i = 1, #stars do
            local s = stars[i]
            s.z = s.z - 2
            if s.z <= 0 then
                s.x = math.random(-300, 300)
                s.y = math.random(-300, 300)
                s.z = 400
            end
            local k = 120.0 / s.z
            local sx = math.floor(sw / 2 + s.x * k)
            local sy = math.floor(sh / 2 + s.y * k)
            if sx >= 0 and sx < sw and sy >= 0 and sy < sh then
                local bright = math.floor((1.0 - (s.z / 400.0)) * 255)
                local col = (bright << 16) | (bright << 8) | bright
                drawRect(sx, sy, 2, 2, col)
            end
        end
        last_mdown = mdown
        _G.needs_redraw = true
        return
    end

    local task_y = sh - 40
    
    -- 2. Draw Desktop Wallpaper Gradient
    local t = themes[current_theme_idx]
    drawGradient(0, 0, sw, task_y, t.c1, t.c2, true)

    -- 3. Draw Desktop Widgets (Clock & Storage indicator)
    local ut = getUptime()
    local h_w = math.floor(ut / 3600)
    local m_w = math.floor((ut / 60) % 60)
    local s_w = math.floor(ut % 60)
    
    local clock_str = string.format("%02d:%02d:%02d", h_w, m_w, s_w)
    drawText(clock_str, sw - 180, 45, 0x51AFEF)
    drawText("System Uptime", sw - 180, 25, 0x5C6370)
    
    local used_ram, total_ram = getMemInfo()
    local used_mb = math.floor(used_ram / (1024 * 1024))
    local total_mb = math.floor(total_ram / (1024 * 1024))
    local ratio = total_ram > 0 and (used_ram / total_ram) or 0
    drawRect(sw - 180, 85, 150, 4, 0x282C34)
    drawRect(sw - 180, 85, math.floor(150 * ratio), 4, 0x98C379)
    drawText(string.format("Memory: %d/%d MB", used_mb, total_mb), sw - 180, 95, 0x98C379)

    -- 4. Keyboard Modifiers Filter
    local key = getLastKey()
    if key > 0 then
        local raw_code = key
        if raw_code >= 0x100 then
            raw_code = raw_code - 0x100
        end

        if raw_code == 42 or raw_code == 54 then
            _G.shift_pressed = true
        elseif raw_code == 170 or raw_code == 182 then
            _G.shift_pressed = false
        elseif raw_code == 29 then
            _G.ctrl_pressed = true
        elseif raw_code == 157 then
            _G.ctrl_pressed = false
        elseif raw_code == 56 then
            _G.alt_pressed = true
        elseif raw_code == 184 then
            _G.alt_pressed = false
        end

        local is_release = (raw_code >= 128 and raw_code <= 255)
        if not is_release then
            local char = scancodeToAscii(key, _G.shift_pressed)
            if focused_window and type(focused_window.handle_key) == "function" then
                focused_window:handle_key(key, char)
            end
        end
    end

    -- 5. Draw Desktop Icons
    for i, icon in ipairs(desktop_icons) do
        local ix, iy = 24, 30 + (i - 1) * 95
        local icon_hover = mx >= ix and mx < ix + 52 and my >= iy and my < iy + 52
        
        drawRect(ix, iy, 52, 52, icon_hover and 0x2C313C or 0x21252B)
        drawRect(ix+2, iy+2, 48, 48, 0x1E2227)
        
        if icon.pixels then
            for row_idx = 1, 44 do
                local row = icon.pixels[row_idx]
                if row then
                    local px, py, k = ix + 4, iy + 2 + row_idx, 1
                    while row[k] do
                        drawRect(px, py, row[k+1], 1, row[k])
                        px = px + row[k+1]
                        k = k + 2
                    end
                end
            end
        else
            drawRect(ix + 4, iy + 4, 44, 44, icon.icon_col)
            if icon.text then drawText(icon.text, ix + 20, iy + 20, 0xFFFFFF) end
        end
        drawText(icon.label, ix + 4, iy + 58, 0xD8DEE9)

        if not dragging_win and not resizing_win and mdown and not last_mdown then
            if icon_hover then
                if icon.exec then
                    exec(icon.exec)
                    if icon.label == "Doom" then
                        for _, w in ipairs(windows) do
                            if w.is_app_container then
                                w.active = true
                                w.minimized = false
                                focused_window = w
                                bring_to_front(w)
                            end
                        end
                    end
                elseif icon.win_title then
                    for _, w in ipairs(windows) do
                        if w.title == icon.win_title then
                            w.active = true
                            w.minimized = false
                            focused_window = w
                            bring_to_front(w)
                        end
                    end
                end
            end
        end
    end

    -- 6. Aero Snap Preview Render & Core drag handling
    if dragging_win then
        if my < 5 then
            drawRect(2, 24, sw-4, sh-70, 0x5C6370)
        elseif mx < 5 then
            drawRect(2, 24, math.floor(sw/2)-2, sh-70, 0x5C6370)
        elseif mx > sw - 5 then
            drawRect(math.floor(sw/2)+2, 24, math.floor(sw/2)-4, sh-70, 0x5C6370)
        end
    end

    if resizing_win then
        if mdown then
            resizing_win.w = mx - resizing_win.x
            resizing_win.h = my - resizing_win.y
            if resizing_win.w < 120 then resizing_win.w = 120 end
            if resizing_win.h < 80 then resizing_win.h = 80 end
            _G.needs_redraw = true
        else
            resizing_win = nil
        end
    end

    if mdown and not last_mdown and not resizing_win then
        local found = false
        for i = #windows, 1, -1 do
            local win = windows[i]
            if win.active and not win.minimized then
                if mx >= win.x and mx < win.x + win.w and my >= win.y - 28 and my < win.y + win.h then
                    focused_window = win
                    bring_to_front(win)
                    
                    if my < win.y then 
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

    if not mdown and dragging_win then
        if my < 5 then
            dragging_win.fullscreen = true
            dragging_win.snapped = false
        elseif mx < 5 then
            dragging_win.snapped = "left"
            dragging_win.fullscreen = false
        elseif mx > sw - 5 then
            dragging_win.snapped = "right"
            dragging_win.fullscreen = false
        else
            dragging_win.fullscreen = false
            dragging_win.snapped = false
        end
        dragging_win = nil
    end

    if dragging_win then
        dragging_win.x = mx - drag_ox
        dragging_win.y = my - drag_oy
        if dragging_win.y < 24 then dragging_win.y = 24 end
        _G.needs_redraw = true
    end

    -- 7. Render Windows
    for _, win in ipairs(windows) do
        win:draw(mx, my, mdown, dt)
    end

    -- 8. Acrylic Start Menu Overlay
    if start_menu_open then
        drawRect(0, sh - 300, 240, 260, 0x21252B)
        drawRect(0, sh - 300, 240, 1, 0x3E4452)
        drawRect(239, sh - 300, 1, 260, 0x3E4452)
        
        drawText("EQUINOX UTILITIES", 15, sh - 285, 0x5C6370)
        
        if button("Kill All Processes", 15, sh - 255, 210, 24) then
            killAllTasks()
            start_menu_open = false
        end
        
        if button("Rotate Workspace Theme", 15, sh - 220, 210, 24) then
            current_theme_idx = current_theme_idx + 1
            if current_theme_idx > #themes then current_theme_idx = 1 end
            _G.needs_redraw = true
        end

        if button("Force Terminal Focus", 15, sh - 185, 210, 24) then
            focused_window = windows[1]
            windows[1].active = true
            windows[1].minimized = false
            bring_to_front(windows[1])
            start_menu_open = false
        end

        local total_tasks = #getTasks()
        drawText(string.format("Active Ring 3 tasks: %d", total_tasks), 15, sh - 145, 0xABB2BF)
        drawText(string.format("GUI Theme: %s", themes[current_theme_idx].name), 15, sh - 120, 0x51AFEF)
    end

    -- 9. macOS-Style Taskbar Dock
    drawGradient(0, task_y, sw, 40, 0x1E222B, 0x16181D, true)
    drawRect(0, task_y, sw, 1, 0x282C34)

    if button("EquinoxOS", 10, task_y + 8, 85, 24) then
        start_menu_open = not start_menu_open
        _G.needs_redraw = true
    end

    local dock_start_x = 110
    for idx, win in ipairs(windows) do
        if win.active and not win.borderless then
            local is_focused = (focused_window == win)
            local rx = dock_start_x + (idx - 1) * 115
            
            local scale_factor = 0
            if mx >= rx and mx < rx + 110 and my >= task_y then
                scale_factor = 4
            end

            drawRect(rx, task_y + 6 - scale_factor, 110, 28 + scale_factor, is_focused and 0x51AFEF or 0x2C313C)
            drawText(string.sub(win.title, 1, 11), rx + 8, task_y + 14 - math.floor(scale_factor/2), 0xFFFFFF)
            
            if not win.minimized then
                drawRect(rx + 50, task_y + 36, 10, 2, 0x98C379)
            end

            if mdown and not last_mdown and mx >= rx and mx < rx + 110 and my >= task_y + 6 then
                if win.minimized then
                    win.minimized = false
                end
                focused_window = win
                bring_to_front(win)
                start_menu_open = false
            end
        end
    end

    local app_focused = (focused_window == app_container)

    -- Если игра закрыта, свернута ИЛИ не в фокусе — сбрасываем координаты в 0
    if not (app_container.active and not app_container.minimized and app_focused) then
        if type(setAppWindowPos) == "function" then
            setAppWindowPos(0, 0, 0, 0)
        end
    end

    last_mx = mx
    last_my = my
    last_mdown = mdown
end

table.insert(windows, app_container)