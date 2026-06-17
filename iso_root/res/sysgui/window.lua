local Window = {}
Window.__index = Window

function Window.new(title, x, y, w, h, draw_cb, key_cb)
    local self = setmetatable({}, Window)
    self.title = title
    self.x = x
    self.y = y
    self.w = w
    self.h = h
    self.old_x = x
    self.old_y = y
    self.old_w = w
    self.old_h = h
    self.active = true
    self.borderless = false
    self.fullscreen = false
    self.is_app_container = false
    self.minimized = false
    self.snapped = false -- false, "left", "right"

    if type(draw_cb) == "table" then
        self.draw_cb = draw_cb.draw
        self.key_cb = draw_cb.handle_key
    else
        self.draw_cb = draw_cb
        self.key_cb = key_cb
    end

    if type(animCreate) == "function" then
        self.anim_scale = animCreate(180, 3) -- Smooth EID_EASE_OUT_CUBIC
        animTo(self.anim_scale, 1.0)
    end

    return self
end

function Window:draw(mx, my, mdown, dt)
    if not self.active or self.minimized then return end

    local sw, sh = getScreenSize()
    local scale = 1.0
    if self.anim_scale then
        animStep(self.anim_scale, dt)
        scale = animEval(self.anim_scale)
    end

    local cx = self.x + self.w / 2
    local cy = self.y + self.h / 2
    local win_w = math.floor(self.w * scale)
    local win_h = math.floor(self.h * scale)
    local win_x = math.floor(cx - win_w / 2)
    local win_y = math.floor(cy - win_h / 2)

    if self.fullscreen then
        win_x, win_y = 0, 24
        win_w, win_h = sw, sh - 88
    elseif self.snapped == "left" then
        win_x, win_y = 0, 24
        win_w, win_h = math.floor(sw / 2), sh - 88
    elseif self.snapped == "right" then
        win_x, win_y = math.floor(sw / 2), 24
        win_w, win_h = math.floor(sw / 2), sh - 88
    end

    local active = (focused_window == self)
    
    if not self.borderless then
        local ty = win_y - 28
        
        -- macOS Liquid Glass: Накладываем Акриловое скругление с неоновой фаской НА ВСЁ ОКНО ЦЕЛИКОМ!
        if type(drawBlur) == "function" then
            local tint = active and 0x1E222B or 0x14161D
            drawBlur(win_x, ty, win_w, win_h + 28, 0.40, 12, tint)
        else
            -- Fallback
            drawRect(win_x, ty, win_w, win_h + 28, active and 0x21252B or 0x1E2227)
        end

        -- КОНТРОЛЛЕР СВЕТОФОРОВ (Traffic Lights) в левом углу
        local r_cx, r_cy = win_x + 16, ty + 14
        local y_cx, y_cy = win_x + 32, ty + 14
        local g_cx, g_cy = win_x + 48, ty + 14

        local mouse_on_lights = (mx >= win_x + 8 and mx < win_x + 56 and my >= ty + 6 and my < ty + 22)

        if type(drawCircle) == "function" then
            drawCircle(r_cx, r_cy, 6, 0xFF5F56, true) -- Красный (Close)
            drawCircle(y_cx, y_cy, 6, 0xFFBD2E, true) -- Желтый (Minimize)
            drawCircle(g_cx, g_cy, 6, 0x27C93F, true) -- Зеленый (Maximize)

            -- Символы x - + внутри кнопок при наведении
            if mouse_on_lights then
                drawText("x", r_cx - 4, r_cy - 7, 0x5C0000)
                drawText("-", y_cx - 4, y_cy - 7, 0x5C4300)
                drawText("+", g_cx - 4, g_cy - 7, 0x004700)
            end
        else
            -- Текстовый фаллбэк если круги не поддерживаются
            drawText("x - +", win_x + 12, ty + 8, 0xFFFFFF)
        end

        -- Логика нажатий на Светофоры
        if mdown and not last_mdown then
            if mx >= r_cx - 6 and mx <= r_cx + 6 and my >= r_cy - 6 and my <= r_cy + 6 then
                -- Закрыть
                self.active = false
                _G.needs_redraw = true
                if self.is_app_container then
                    if type(getTasks) == "function" and type(killTask) == "function" then
                        local tasks = getTasks()
                        for _, t in ipairs(tasks) do
                            if t.pid ~= 1 and t.pid ~= 2 and t.pid ~= 3 then
                                killTask(t.pid)
                            end
                        end
                    end
                end
                return
            elseif mx >= y_cx - 6 and mx <= y_cx + 6 and my >= y_cy - 6 and my <= y_cy + 6 then
                -- Свернуть
                self.minimized = true
                _G.needs_redraw = true
                return
            elseif mx >= g_cx - 6 and mx <= g_cx + 6 and my >= g_cy - 6 and my <= g_cy + 6 then
                -- На весь экран / Сбросить
                if self.fullscreen or self.snapped then
                    self.fullscreen = false
                    self.snapped = false
                    self.x, self.y, self.w, self.h = self.old_x, self.old_y, self.old_w, self.old_h
                else
                    self.old_x, self.old_y, self.old_w, self.old_h = self.x, self.y, self.w, self.h
                    self.fullscreen = true
                end
                _G.needs_redraw = true
            end
        end

        -- Текст заголовка окна строго по центру (Стиль macOS)
        local title_len = #self.title * 8
        local tx = win_x + math.floor((win_w - title_len) / 2)
        drawText(self.title, tx, ty + 8, active and 0xFFFFFF or 0xABB2BF)
    end

    -- Отрисовка тела окна
    if not self.is_app_container then
        if self.draw_cb then 
            local original_x, original_y = self.x, self.y
            local original_w, original_h = self.w, self.h
            self.x, self.y = win_x, win_y
            self.w, self.h = win_w, win_h

            self.draw_cb(self, mx, my, mdown, dt) 

            self.x, self.y = original_x, original_y
            self.w, self.h = original_w, original_h
        end
    else
        if type(setAppWindowPos) == "function" then
            if focused_window == self and win_w > 0 and win_h > 0 then
                setAppWindowPos(win_x, win_y, win_w, win_h)
            end
        end
    end

    -- Угловой Resize Handle
    if not self.borderless and not self.fullscreen and not self.snapped and scale >= 0.95 then
        local rx, ry = win_x + win_w - 10, win_y + win_h - 10
        drawRect(rx, ry, 10, 10, active and 0x0078D7 or 0x4E5666)
        if mdown and not last_mdown and mx >= rx and mx < rx+10 and my >= ry and my < ry+10 then
            resizing_win = self
        end
    end
end

function Window:handle_key(key, char)
    if self.active and self.key_cb then
        self.key_cb(self, key, char)
    end
end

return Window