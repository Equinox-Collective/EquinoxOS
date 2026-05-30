--- START OF FILE res/sysgui/window.lua ---
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
    self.snapped = false -- false, "left", "right", "top"

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
        win_w, win_h = sw, sh - 56
    elseif self.snapped == "left" then
        win_x, win_y = 0, 24
        win_w, win_h = math.floor(sw / 2), sh - 56
    elseif self.snapped == "right" then
        win_x, win_y = math.floor(sw / 2), 24
        win_w, win_h = math.floor(sw / 2), sh - 56
    end

    local active = (focused_window == self)
    
    if not self.borderless then
        local ty = win_y - 28
        
        -- Software Drop Shadow Simulation
        drawRect(win_x - 4, ty - 4, win_w + 8, win_h + 36, 0x050608)
        drawRect(win_x - 2, ty - 2, win_w + 4, win_h + 32, 0x0E0F12)

        -- Border & Selection Highlights
        local border_color = active and 0x0078D7 or 0x3E4452
        drawRect(win_x - 1, ty - 1, win_w + 2, win_h + 30, border_color)

        -- Acrylic Style Title bar
        if type(drawBlur) == "function" then
            drawBlur(win_x, ty, win_w, 28, active and 0.50 or 0.40)
        else
            drawRect(win_x, ty, win_w, 28, active and 0x21252B or 0x1E2227)
        end

        -- Active light-bar accent on title top
        if active then
            drawRect(win_x, ty, win_w, 1, 0x0078D7)
        end

        -- Window title text
        drawText(self.title, win_x + 10, ty + 8, active and 0xFFFFFF or 0xABB2BF)

        -- Action buttons: Minimize [ - ], Maximize [ [] ], Close [ X ]
        local bx = win_x + win_w - 76
        
        -- Minimize Button
        if button("_", bx, ty + 4, 20, 20) then
            self.minimized = true
            _G.needs_redraw = true
            return
        end
        
        -- Maximize/Restore Button
        if button("O", bx + 24, ty + 4, 20, 20) then
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

        -- Close Button
        if button("X", bx + 48, ty + 4, 20, 20) then
            self.active = false
            _G.needs_redraw = true
            return
        end

        drawRect(win_x, win_y - 1, win_w, 1, active and 0x0078D7 or 0x3E4452)
    end

    -- Window Body
    if not self.is_app_container then
        if not self.borderless then
            if type(drawBlur) == "function" then
                drawBlur(win_x, win_y, win_w, win_h, 0.70)
            else
                drawRect(win_x, win_y, win_w, win_h, 0x1E1E24)
            end
        end
        
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
            setAppWindowPos(win_x, win_y, win_w, win_h)
        end
    end

    -- Resize Handle (Visible when fully opened)
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
--- END OF FILE res/sysgui/window.lua ---