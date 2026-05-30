-- res/sysgui/monitor.lua
local M = {}

-- Таблица для хранения истории использования ОЗУ (последние 45 замеров)
local ram_history = {}
local max_points = 45
local last_update_time = 0

M.draw = function(win, mx, my, mdown, dt)
    local used, total = getMemInfo()
    local used_mb = math.floor(used / (1024 * 1024))
    local total_mb = math.floor(total / (1024 * 1024))
    local ratio = used / total

    -- 1. Снимаем показания раз в 250 мс для формирования графика
    local now = getUptime()
    if now - last_update_time >= 0.25 then
        table.insert(ram_history, ratio)
        if #ram_history > max_points then
            table.remove(ram_history, 1) -- Удаляем самый старый замер
        end
        last_update_time = now
    end

    -- 2. Вывод текстовой информации
    local ram_str = string.format("RAM: %d / %d MB (%d%%)", used_mb, total_mb, math.floor(ratio * 100))
    drawText(ram_str, win.x + 15, win.y + 16, 0x8BE9FD)

    -- 3. Отрисовка сетки монитора (Grid)
    local graph_x = win.x + 15
    local graph_y = win.y + 40
    local graph_w = win.w - 30
    local graph_h = win.h - 55

    -- Задний фон графика
    drawRect(graph_x, graph_y, graph_w, graph_h, 0x14151B)

    -- Горизонтальные линии сетки
    for i = 1, 3 do
        local ly = graph_y + math.floor(graph_h * i / 4)
        drawLine(graph_x, ly, graph_x + graph_w - 1, ly, 0x21222C)
    end

    -- Вертикальные линии сетки
    for i = 1, 5 do
        local lx = graph_x + math.floor(graph_w * i / 6)
        drawLine(lx, graph_y, lx, graph_y + graph_h - 1, 0x21222C)
    end

    -- 4. Отрисовка бегущего графика (Line Chart)
    if #ram_history > 1 then
        local step = graph_w / (max_points - 1)
        
        for i = 1, #ram_history - 1 do
            -- Координаты текущей точки
            local x1 = graph_x + math.floor((i - 1) * step)
            -- Ограничиваем график высотой, инвертируем Y (так как 0 сверху)
            local y1 = graph_y + graph_h - math.floor(ram_history[i] * (graph_h - 4)) - 2

            -- Координаты следующей точки
            local x2 = graph_x + math.floor(i * step)
            local y2 = graph_y + graph_h - math.floor(ram_history[i+1] * (graph_h - 4)) - 2

            -- Рисуем линию графика (красивым неоновым цветом)
            drawLine(x1, y1, x2, y2, 0x50FA7B)
        end
    end
end

-- Монитор работает пассивно, клавиатурный ввод ему не нужен
M.handle_key = function(win, key, char)
end

return M