print("enGUI Desktop Environment: Loading macOS Liquid Glass core...")

windows = {}
focused_window = nil
last_mdown = false
resizing_win = nil

_G.needs_redraw = false
_G.shift_pressed = false
_G.ctrl_pressed = false
_G.alt_pressed = false

local Window = dofile("res/sysgui/window.lua")

-- ЛЕНИВАЯ ЗАГРУЗКА ПРИЛОЖЕНИЙ
local function _make_lazy(path)
    local mod = nil
    return function()
        if mod == nil then mod = dofile(path) end
        return mod
    end
end

local function lazy_win(path)
    local get = _make_lazy(path)
    local draw_cb = function(...)
        local m = get()
        if type(m) == "table" then return m.draw(...) end
        return m(...)
    end
    local key_cb = function(...)
        local m = get()
        if type(m) == "table" and m.handle_key then return m.handle_key(...) end
    end
    return draw_cb, key_cb
end

local draw_terminal, key_terminal = lazy_win("res/sysgui/terminal.lua")
local draw_notepad, key_notepad = lazy_win("res/sysgui/notepad.lua")
local draw_monitor, key_monitor = lazy_win("res/sysgui/monitor.lua")
local draw_paint, key_paint = lazy_win("res/sysgui/paint.lua")
local draw_explorer = dofile("res/sysgui/explorer.lua")

local app_container = Window.new("External Application", 250, 150, 640, 400, nil)
app_container.is_app_container = true
app_container.active = false

-- Состояние интро
local system_state = "BOOT"
local bootvid = nil
local bootvid_ok, res = pcall(dofile, "res/sysgui/bootvid.lua")

if bootvid_ok and type(res) == "table" and type(res.init) == "function" then
    bootvid = res
    bootvid.init()
else
    system_state = "DESKTOP"
end

-- Скринсейвер
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

-- Темы рабочего стола (Градиенты macOS Big Sur / Sonoma)
local themes = {
    { c1 = 0x1A1C29, c2 = 0x0E1017, name = "Sonoma Dark" },
    { c1 = 0x1E102F, c2 = 0x0A0510, name = "Nebula Purple" },
    { c1 = 0x0B1D20, c2 = 0x04090A, name = "Aqua Marin" }
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

-- Иконки macOS Дока
local desktop_icons = {
    { label = "Terminal", icon_col = 0x21252B, text = ">_", win_title = "Equinox Terminal" },
    { label = "Monitor",  icon_col = 0x4B5263, text = "M",  win_title = "System Monitor" },
    { label = "Paint",    icon_col = 0xFF8700, text = "P",  win_title = "Vector Paint Brush" },
    { label = "Explorer", icon_col = 0xE5C07B, text = "E",  win_title = "VFS File Explorer" },
    { label = "Notepad",  icon_col = 0x61AFEF, text = "N",  win_title = "Notepad Text Editor" },
    { label = "Doom",     icon_col = 0xE06C75, text = "",   exec = "bin/doom.elf -iwad res/doom1.wad", pixels = doom_pixels },
}

table.insert(windows, Window.new("Equinox Terminal", 50, 80, 520, 340, draw_terminal, key_terminal))
table.insert(windows, Window.new("System Monitor", 620, 80, 340, 220, draw_monitor, key_monitor))
table.insert(windows, Window.new("Vector Paint Brush", 120, 200, 440, 320, draw_paint, key_paint))
table.insert(windows, Window.new("VFS File Explorer", 400, 150, 360, 280, draw_explorer))
table.insert(windows, Window.new("Notepad Text Editor", 100, 100, 420, 280, draw_notepad, key_notepad))

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
    -- --- 0. ЗАГРУЗОЧНАЯ АНИМАЦИЯ ---
    if system_state == "BOOT" and bootvid then
        _G.needs_redraw = true 
        local is_finished = bootvid.draw(dt)
        if is_finished then
            system_state = "DESKTOP"
            _G.needs_redraw = true 
        end
        return
    end

    local sw, sh = getScreenSize() 
    local mx, my, mdown = getMouse()
    local key = getLastKey()

    -- 1. Скринсейвер
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
            if t.pid ~= 1 and t.pid ~= 2 then
                external_proc_alive = true
            end
        end
        if not external_proc_alive then
            app_container.active = false
            focused_window = nil
            _G.needs_redraw = true 
        end
    end

    if mx ~= last_mx or my ~= last_my or mdown ~= last_mdown or key > 0 or is_app_running then
        last_input_time = getUptime()
        if screensaver_active then
            screensaver_active = false
            _G.needs_redraw = true
        end
    end

    if getUptime() - last_input_time > 15 then
        screensaver_active = true
    end

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

    -- 2. Обои (маковский глубокий Sonoma градиент)
    local t = themes[current_theme_idx]
    drawGradient(0, 0, sw, sh, t.c1, t.c2, true)

    -- 3. Клавиатурные модификаторы
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

    -- 4. Изменение размеров и перетягивание окон (Aero Snap)
    if dragging_win then
        if my < 24 then
            drawRect(2, 24, sw-4, sh-84, 0x5C6370)
        elseif mx < 5 then
            drawRect(2, 24, math.floor(sw/2)-2, sh-84, 0x5C6370)
        elseif mx > sw - 5 then
            drawRect(math.floor(sw/2)+2, 24, math.floor(sw/2)-4, sh-84, 0x5C6370)
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
        if not found and my > 24 and my < sh - 75 then focused_window = nil end
    end

    if not mdown and dragging_win then
        if my < 28 then
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

    -- 5. Рендеринг всех окон (с новым эффектом Liquid Glass)
    for _, win in ipairs(windows) do
        win:draw(mx, my, mdown, dt)
    end

    -- 6. macOS-Style Top Menu Bar (Размытая стеклянная плашка во весь экран)
    drawBlur(0, 0, sw, 24, 0.35, 0, 0x1E222B)
    drawRect(0, 23, sw, 1, 0x2C313C) -- Тонкая разделительная фаска

    -- Системная кнопка Apple/Equinox и меню активного приложения
    if button("EQ", 10, 4, 30, 16) then
        start_menu_open = not start_menu_open
        _G.needs_redraw = true
    end

    local active_title = focused_window and focused_window.title or "Finder"
    drawText(active_title, 55, 6, 0xFFFFFF)

    -- ЧАСЫ и системный трей справа
    local ut = getUptime()
    local h_w = math.floor(ut / 3600)
    local m_w = math.floor((ut / 60) % 60)
    local s_w = math.floor(ut % 60)
    local clock_str = string.format("%02d:%02d:%02d", h_w, m_w, s_w)
    
    drawText(clock_str, sw - 80, 6, 0xFFFFFF)

    local used_ram, total_ram = getMemInfo()
    local used_mb = math.floor(used_ram / (1024 * 1024))
    local total_mb = math.floor(total_ram / (1024 * 1024))
    drawText(string.format("Memory: %d/%d MB", used_mb, total_mb), sw - 210, 6, 0x98C379)

    -- 7. macOS-Style Floating Dock (Центрированный с эффектом Magnification)
    local dock_h = 52
    local icon_size_base = 40
    local gap = 12
    local total_icons = #desktop_icons
    
    -- Считаем ширину дока
    local dock_w = total_icons * (icon_size_base + gap) + gap
    local dock_x = math.floor((sw - dock_w) / 2)
    local dock_y = sh - dock_h - 12 -- Оставляем 12px отступ снизу
    
    -- Рисуем размытую стеклянную подложку дока скруглением 14px
    drawBlur(dock_x, dock_y, dock_w, dock_h, 0.45, 14, 0x1E222B)

    -- Отрисовка иконок в доке
    for i, icon in ipairs(desktop_icons) do
        -- Вычисление центра базовой позиции иконки
        local base_cx = dock_x + gap + (i - 1) * (icon_size_base + gap) + icon_size_base / 2
        local dist_x = math.abs(mx - base_cx)
        
        -- Вычисляем увеличение при приближении мыши (Hover Magnification)
        local scale = 1.0
        if my >= dock_y - 15 and my <= sh and dist_x < 80 then
            scale = 1.0 + (1.0 - (dist_x / 80)) * 0.35 -- Увеличиваем до 35% при наведении
        end
        
        local size = math.floor(icon_size_base * scale)
        local ix = math.floor(base_cx - size / 2)
        local iy = math.floor(dock_y + (dock_h - size) / 2)
        
        local icon_hover = mx >= ix and mx < ix + size and my >= iy and my < iy + size

        -- Фон иконки (круг или скругленный квадрат)
        drawRect(ix, iy, size, size, icon_hover and 0x2C313C or 0x21252B)
        drawRect(ix+2, iy+2, size-4, size-4, 0x1E2227)
        
        if icon.pixels then
            -- Рендеринг иконки DOOM (масштабирование под размер)
            local p_scale = size / 48
            for row_idx = 1, 44 do
                local row = icon.pixels[row_idx]
                if row then
                    local py = iy + math.floor((2 + row_idx) * p_scale)
                    local px = ix + math.floor(4 * p_scale)
                    local k = 1
                    while row[k] do
                        local w_scaled = math.floor(row[k+1] * p_scale)
                        if w_scaled <= 0 then w_scaled = 1 end
                        drawRect(px, py, w_scaled, 1, row[k])
                        px = px + w_scaled
                        k = k + 2
                    end
                end
            end
        else
            -- Стандартная иконка приложений
            drawRect(ix + 4, iy + 4, size - 8, size - 8, icon.icon_col)
            if icon.text and size > 30 then
                drawText(icon.text, ix + math.floor(size/2) - 4, iy + math.floor(size/2) - 4, 0xFFFFFF)
            end
        end

        -- Текст-подсказка названия приложения при наведении
        if icon_hover then
            local lbl_w = #icon.label * 8
            local lbl_x = math.floor(base_cx - lbl_w/2)
            drawBlur(lbl_x - 6, dock_y - 30, lbl_w + 12, 18, 0.70, 6, 0x1E222B)
            drawText(icon.label, lbl_x, dock_y - 25, 0xFFFFFF)
        end

        -- Светящийся индикатор выполнения внизу иконки (как на macOS)
        local is_running = false
        if icon.win_title then
            for _, w in ipairs(windows) do
                if w.title == icon.win_title and w.active then
                    is_running = true
                    break
                end
            end
        elseif icon.label == "Doom" and app_container.active then
            is_running = true
        end

        if is_running then
            -- Отрисовка маленькой белой точки под запущенным приложением
            drawRect(base_cx - 2, sh - 16, 4, 2, 0xFFFFFF)
        end

        -- Логика клика/запуска приложения
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

    -- 8. Акриловое выпадающее меню Системы (Apple Menu)
    if start_menu_open then
        drawBlur(10, 28, 240, 150, 0.65, 10, 0x21252B)
        
        if button("Force Restart Desk", 25, 45, 210, 24) then
            killAllTasks()
            start_menu_open = false
        end
        
        if button("Switch Desktop Theme", 25, 80, 210, 24) then
            current_theme_idx = current_theme_idx + 1
            if current_theme_idx > #themes then current_theme_idx = 1 end
            _G.needs_redraw = true
        end

        if button("Open Terminal Window", 25, 115, 210, 24) then
            focused_window = windows[1]
            windows[1].active = true
            windows[1].minimized = false
            bring_to_front(windows[1])
            start_menu_open = false
        end

        local total_tasks = #getTasks()
        drawText(string.format("Ring 3 Tasks: %d", total_tasks), 25, 155, 0xABB2BF)
    end

    -- Сброс положения кадра для doom.elf
    local app_focused = (focused_window == app_container)
    local any_external_proc = false
    do
        local tasks = getTasks()
        for _, t in ipairs(tasks) do
            if t.pid ~= 1 and t.pid ~= 2 then
                any_external_proc = true
                break
            end
        end
    end

    if not (app_container.active and not app_container.minimized and app_focused)
       and not any_external_proc then
        if type(setAppWindowPos) == "function" then
            setAppWindowPos(0, 0, 0, 0)
        end
    end

    last_mx = mx
    last_my = my
    last_mdown = mdown
end

table.insert(windows, app_container)