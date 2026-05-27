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
    self.borderless = false   -- Без рамок и заголовка
    self.fullscreen = false   -- На весь экран
    self.is_app_container = false
    self.draw_cb = draw_cb
    return self
end

function Window:draw(mx, my, mdown, dt)
    if not self.active then return end

    local sw, sh = getScreenSize()
    
    if self.fullscreen then
        self.x, self.y = 0, 0
        self.w, self.h = sw, sh
    end

    local active = (focused_window == self)
    
    -- Рендеринг рамок окна
    if not self.borderless and not self.fullscreen then
        local ty = self.y - 28
        -- Тень
        drawRect(self.x + 2, ty + 2, self.w + 4, self.h + 30, 0x000000AA) 
        -- Обводка
        drawRect(self.x - 1, ty - 1, self.w + 2, self.h + 30, active and 0x555555 or 0x333333)
        -- Заголовок
        if active then
            drawGradient(self.x, ty, self.w, 28, 0x1A72BB, 0x0E4581, true)
        else
            drawGradient(self.x, ty, self.w, 28, 0x2D2D30, 0x1E1E1E, true)
        end
        -- Текст и кнопка закрытия
        drawText(self.title, self.x + 10, ty + 8, 0xFFFFFF)
        local bx = self.x + self.w - 26
        if button("X", bx, ty + 4, 22, 20) then
            self.active = false
            return
        end
        -- Разделитель
        drawRect(self.x, self.y - 1, self.w, 1, active and 0x55AFFF or 0x444444)
    end

    -- Рендеринг контента
    if not self.is_app_container then
        if not self.borderless then
            drawRect(self.x, self.y, self.w, self.h, 0x1E1E1E)
        end
        
        if self.draw_cb then 
            self.draw_cb(self, mx, my, mdown, dt) 
        end
    else
        -- Контейнер для внешних ELF (Doom/Snake)
        if type(setAppWindowPos) == "function" then
            setAppWindowPos(self.x, self.y, self.w, self.h)
        end
    end

    -- Resize Handle
    if not self.borderless and not self.fullscreen then
        local rx, ry = self.x + self.w - 10, self.y + self.h - 10
        drawRect(rx, ry, 10, 10, active and 0x0078D7 or 0x444444)
        if mdown and not last_mdown and mx >= rx and mx < rx+10 and my >= ry and my < ry+10 then
            resizing_win = self
        end
    end
end

return Window