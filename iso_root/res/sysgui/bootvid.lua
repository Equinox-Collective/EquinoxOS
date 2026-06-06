local bootvid = {}

-- ============================================================================
--  НАСТРОЙКА ЗВУКА ПРИ СТАРТЕ СИСТЕМЫ  (boot-конфиг)
--  ---------------------------------------------------------------------------
--  Хочешь выключить музыку при загрузке ОС — поставь здесь false и пересобери
--  (make all). true = играть звук запуска, false = тишина.
--
--  Это ГЛОБАЛЬНАЯ переменная: её читает код на C (api_preload_boot_sound в
--  api_gui.c) до запуска рабочего стола. Звук грузится с диска заранее (под
--  ещё крутящимся сплэшем) и стартует, как только готова звуковая карта AC'97.
-- ============================================================================
BOOT_SOUND_ENABLED = true

-- === БЫСТРАЯ ЗАГРУЗКА ===
-- Раньше сплэш искусственно держался ~6.5 c (1.2 c появление + 4.5 c удержание
-- "под длину звука" + 0.8 c растворение) и СИНХРОННО грузил WAV с диска через
-- ATA PIO, тормозя первый кадр рабочего стола.
-- FAST_BOOT: короткий брендинг и без блокировки на звуке.
-- ЗВУК ЗАПУСКА проигрывается из C (api_preload_boot_sound + api_try_boot_sound
-- в main.c) ровно один раз. Поэтому Lua-проигрывание PLAY_BOOT_SOUND здесь
-- ВЫКЛЮЧЕНО, чтобы звук не запускался дважды. Пользовательский переключатель
-- музыки — это глобальная BOOT_SOUND_ENABLED выше.
local FAST_BOOT       = true
local PLAY_BOOT_SOUND = false
local FADE_IN_MS  = FAST_BOOT and 250  or 1200
local HOLD_MS     = FAST_BOOT and 150  or 4500
local FADE_OUT_MS = FAST_BOOT and 250  or 800

local alpha_anim = nil
local fade_out_anim = nil
local hold_timer = 0
local finished = false
local sound_started = false

function bootvid.init()
    if type(animCreate) == "function" then
        alpha_anim = animCreate(FADE_IN_MS, 3) -- Плавное появление
        if type(animTo) == "function" then
            animTo(alpha_anim, 255)
        end
    end
end

function bootvid.draw(dt)
    local sw, sh = 1024, 768
    if type(getScreenSize) == "function" then
        sw, sh = getScreenSize()
    end
    
    -- Очищаем экран глубоким темным цветом
    if type(drawRect) == "function" then
        drawRect(0, 0, sw, sh, 0x07080B)
    end
    
    -- Запускаем звук один раз на старте (по умолчанию выключено для скорости:
    -- загрузка 846КБ WAV с диска блокирует первый кадр).
    if PLAY_BOOT_SOUND and not sound_started then
        if type(playSound) == "function" then
            playSound("res/sysgui/BOOTSOUND.wav")
        end
        sound_started = true
    end

    -- Обновляем анимацию появления (умножаем на 255)
    local alpha = 0
    if alpha_anim then
        if type(animStep) == "function" then
            animStep(alpha_anim, dt)
        end
        if type(animEval) == "function" then
            alpha = math.floor(animEval(alpha_anim) * 255)
            if alpha < 0 then alpha = 0 end
            if alpha > 255 then alpha = 255 end
        end
    else
        alpha = 255
    end
    
    -- Безопасное цветосмешение без побитовых сдвигов
    local function blend(c)
        local r_bg, g_bg, b_bg = 0x07, 0x08, 0x0B
        local r_fg = math.floor(c / 65536) % 256
        local g_fg = math.floor(c / 256) % 256
        local b_fg = c % 256
        
        local r = math.floor(r_fg * (alpha / 255) + r_bg * (1 - alpha / 255))
        local g = math.floor(g_fg * (alpha / 255) + g_bg * (1 - alpha / 255))
        local b = math.floor(b_fg * (alpha / 255) + b_bg * (1 - alpha / 255))
        
        return r * 65536 + g * 256 + b
    end
    
    local col_text1 = blend(0xFFFFFF) -- Белый
    local col_text2 = blend(0x5C6370) -- Серый
    
    -- Текст позиционируется ровно по центру экрана
    local lx = math.floor(sw / 2)
    local ly = math.floor(sh / 2) - 20

    if type(drawText) == "function" then
        drawText("E Q U I N O X   O S", lx - 80, ly, col_text1)
        drawText("loading modular core...", lx - 90, ly + 30, col_text2)
    end
    
    -- Фаза удержания и растворения
    if alpha >= 254 then
        hold_timer = hold_timer + dt
        if hold_timer > HOLD_MS then -- Длительность удержания текста
            if not fade_out_anim and type(animCreate) == "function" then
                fade_out_anim = animCreate(FADE_OUT_MS, 3)
                if type(animTo) == "function" then
                    animTo(fade_out_anim, 0)
                end
            end
            
            if fade_out_anim then
                if type(animStep) == "function" then
                    animStep(fade_out_anim, dt)
                end
                local fade_alpha = 0
                if type(animEval) == "function" then
                    fade_alpha = math.floor(animEval(fade_out_anim) * 255)
                    if fade_alpha < 0 then fade_alpha = 0 end
                    if fade_alpha > 255 then fade_alpha = 255 end
                end
                if fade_alpha <= 2 then
                    finished = true
                end
                alpha_anim = fade_out_anim
            else
                finished = true
            end
        end
    end
    
    return finished
end

return bootvid