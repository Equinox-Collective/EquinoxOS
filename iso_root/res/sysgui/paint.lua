-- res/sysgui/paint.lua
local paint_strokes = {} 
local active_stroke = nil
local active_color = 0xFF0000

local palette = {
    0x000000, 0xFF0000, 0x00FF00, 0x0000FF,
    0xFFFF00, 0xFF00FF, 0x00FFFF, 0xFFFFFF
}

local function draw_paint(win, mx, my, mdown, dt)
    -- Панель
    drawRect(win.x, win.y, win.w, 24, 0x2E303B)
    drawRect(win.x, win.y + 24, win.w, 1, 0x4B5263)

    -- Палитра
    for idx, col in ipairs(palette) do
        local px = win.x + 4 + (idx - 1) * 22
        drawRect(px, win.y + 3, 18, 18, col)
        
        if mx >= px and mx < px + 18 and my >= win.y + 3 and my < win.y + 21 then
            if mdown and not last_mdown then
                active_color = col
            end
        end
    end

    -- Кнопка CLR
    local clr_x = win.x + win.w - 110
    if button("CLR", clr_x, win.y + 3, 45, 18) then
        paint_strokes = {}
    end

    -- Кнопка SAVE
    local save_x = win.x + win.w - 55
    if button("SAVE", save_x, win.y + 3, 50, 18) then
        saveFile("PAINT.TXT", "Equinox Paint Canvas Dump")
    end

    -- Координаты холста
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

    -- Ограничиваем рендеринг рамками холста
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

return draw_paint