-- enGUI: Lua System UI
print("enGUI: init.lua started successfully!")

-- Хранилище настроек
local settings = {
    enable_wifi = false,
    enable_blur = true,
    volume = 0.5,
}

-- Инициализируем анимацию выезжающей панели
-- Длительность 300мс, Ease Out Cubic (тип 3)
local panel_anim = animCreate(300.0, 3) 
animTo(panel_anim, 1.0) -- Начинаем анимацию открытия (от 0 до 1)

-- Функция срабатывает каждые ~16мс
function on_tick(dt)
    -- Шагаем анимацию
    animStep(panel_anim, dt)
    local t = animEval(panel_anim)

    -- Очищаем экран (красивый темный фон)
    drawRect(0, 0, 1024, 768, 0x14161B)

    -- Вычисляем положение выезжающей боковой панели (выезжает слева)
    local panel_w = 320
    local panel_x = math.floor((t - 1.0) * panel_w)

    -- Рисуем фон панели градиентом
    drawGradient(panel_x, 0, panel_w, 768, 0x1E222B, 0x15181F, true)

    -- Текст заголовка
    drawText("Equinox Control Center", panel_x + 20, 30, 0xFFFFFF)
    drawRect(panel_x, 65, panel_w, 1, 0x2A2E3D) -- Разделительная линия

    -- 1. Чекбоксы (Возвращают новое состояние!)
    settings.enable_wifi = checkbox("Enable Wi-Fi Network", panel_x + 20, 90, settings.enable_wifi)
    settings.enable_blur = checkbox("Enable Glass Blur Effect", panel_x + 20, 130, settings.enable_blur)

    -- 2. Слайдер громкости
    drawText("System Volume", panel_x + 20, 180, 0x8A8E9B)
    settings.volume = slider("vol_slider", panel_x + 20, 205, 260, settings.volume, 0.0, 1.0)

    -- 3. Кнопка сброса настроек
    if button("Reset Options", panel_x + 20, 260, 140, 30) then
        settings.enable_wifi = false
        settings.enable_blur = true
        settings.volume = 0.5
        -- Перезапускаем анимацию панели для вау-эффекта
        animTo(panel_anim, 0.0)
        -- И через секунду открываем обратно
        -- (Для сложных таймеров можно сделать логику в Lua)
        animTo(panel_anim, 1.0)
    end

    -- Статусная строка внизу панели
    local status = "Status: OK"
    if settings.enable_wifi then
        status = "Status: Connecting..."
    end
    drawText(status, panel_x + 20, 720, 0x4A8DFD)
end