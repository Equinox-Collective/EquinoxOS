--- START OF FILE res/sysgui/paint.lua ---
local paint_strokes = {} 
local active_stroke = nil
local active_color = 0xE06C75
local brush_size = 2.0
local draw_mode = "PENCIL" -- PENCIL, LINE, RECT, ERASER

local palette = {
    0x282C34, 0xE06C75, 0x98C379, 0xE5C07B,
    0x61AFEF, 0xC678DD, 0x56B6C2, 0xABB2BF
}

local function draw_paint(win, mx, my, mdown, dt)
    -- Toolbar background
    drawRect(win.x, win.y, win.w, 40, 0x21252B)
    drawRect(win.x, win.y + 40, win.w, 1, 0x3E4452)

    -- Dynamic Color Palette selection
    for idx, col in ipairs(palette) do
        local px = win.x + 8 + (idx - 1) * 24
        drawRect(px, win.y + 10, 20, 20, col)
        if col == active_color then
            drawRect(px+4, win.y+14, 12, 12, 0xFFFFFF)
        end
        
        if mx >= px and mx < px + 20 and my >= win.y + 10 and my < win.y + 30 then
            if mdown and not last_mdown then
                active_color = col
            end
        end
    end

    -- Toolbar Mode Buttons
    local tx = win.x + 210
    if button(draw_mode == "PENCIL" and "[Pencil]" or "Pencil", tx, win.y + 8, 55, 24) then
        draw_mode = "PENCIL"
    end
    if button(draw_mode == "LINE" and "[Line]" or "Line", tx + 60, win.y + 8, 45, 24) then
        draw_mode = "LINE"
    end
    if button(draw_mode == "RECT" and "[Rect]" or "Rect", tx + 110, win.y + 8, 45, 24) then
        draw_mode = "RECT"
    end
    if button(draw_mode == "ERASER" and "[Eraser]" or "Eraser", tx + 160, win.y + 8, 55, 24) then
        draw_mode = "ERASER"
    end

    -- Canvas clearing
    if button("CLR", win.x + win.w - 55, win.y + 8, 45, 24) then
        paint_strokes = {}
    end

    -- Interactive Brush Size Slider
    drawText("Size", win.x + 10, win.y + 45, 0x5C6370)
    brush_size = slider("brush_size", win.x + 50, win.y + 45, 120, brush_size, 1.0, 8.0)

    -- Paint Canvas Interactions
    local canvas_y = win.y + 70
    local inside_canvas = (mx >= win.x and mx < win.x + win.w and my >= canvas_y and my < win.y + win.h)
    local is_focused = (focused_window == win)

    if is_focused and inside_canvas and mdown then
        local draw_col = draw_mode == "ERASER" and 0x1E1E24 or active_color
        if not last_mdown then
            active_stroke = {
                color = draw_col,
                mode = draw_mode,
                size = brush_size,
                points = {{ x = mx, y = my }}
            }
            table.insert(paint_strokes, active_stroke)
        else
            if active_stroke then
                if draw_mode == "PENCIL" or draw_mode == "ERASER" then
                    table.insert(active_stroke.points, { x = mx, y = my })
                elseif draw_mode == "LINE" or draw_mode == "RECT" then
                    -- Track anchor and endpoint
                    if #active_stroke.points < 2 then
                        table.insert(active_stroke.points, { x = mx, y = my })
                    else
                        active_stroke.points[2] = { x = mx, y = my }
                    end
                end
            end
        end
    else
        active_stroke = nil
    end

    -- Render Vector shapes inside bounds
    local canvas_x0 = win.x
    local canvas_y0 = canvas_y
    local canvas_x1 = win.x + win.w
    local canvas_y1 = win.y + win.h
    
    local function in_canvas(p)
        return p.x >= canvas_x0 and p.x < canvas_x1 and p.y >= canvas_y0 and p.y < canvas_y1
    end
    
    for _, stroke in ipairs(paint_strokes) do
        local points = stroke.points
        if stroke.mode == "PENCIL" or stroke.mode == "ERASER" then
            for k = 1, #points - 1 do
                local p1 = points[k]
                local p2 = points[k+1]
                if in_canvas(p1) and in_canvas(p2) then
                    drawLine(p1.x, p1.y, p2.x, p2.y, stroke.color)
                    if stroke.size > 2 then
                        drawLine(p1.x+1, p1.y, p2.x+1, p2.y, stroke.color)
                        drawLine(p1.x, p1.y+1, p2.x, p2.y+1, stroke.color)
                    end
                end
            end
        elseif stroke.mode == "LINE" and #points == 2 then
            if in_canvas(points[1]) and in_canvas(points[2]) then
                drawLine(points[1].x, points[1].y, points[2].x, points[2].y, stroke.color)
            end
        elseif stroke.mode == "RECT" and #points == 2 then
            local rx = points[1].x
            local ry = points[1].y
            local rw = points[2].x - rx
            local rh = points[2].y - ry
            if in_canvas(points[1]) and in_canvas(points[2]) then
                drawLine(rx, ry, rx+rw, ry, stroke.color)
                drawLine(rx, ry+rh, rx+rw, ry+rh, stroke.color)
                drawLine(rx, ry, rx, ry+rh, stroke.color)
                drawLine(rx+rw, ry, rx+rw, ry+rh, stroke.color)
            end
        end
    end
end

return draw_paint
--- END OF FILE res/sysgui/paint.lua ---