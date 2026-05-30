-- res/sysgui/window.lua
local Window = {}
Window.__index = Window

-- Добавили key_cb в аргументы для явной передачи, если нужно
function Window.new(title, x, y, w, h, draw_cb, key_cb)
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

    -- УМНАЯ РАСПАКОВКА: проверяем, что нам пришло на вход
    if type(draw_cb) == "table" then
        self.draw_cb = draw_cb.draw          -- Достаем функцию рисования из модуля
        self.key_cb = draw_cb.handle_key     -- Достаем обработчик клавиш из модуля
    else
        self.draw_cb = draw_cb
        self.key_cb = key_cb
    end

    -- Инициализируем аппаратную анимацию масштабирования при создании окна
    if type(animCreate) == "function" then
        -- Создаем аниматор: 200 мс, алгоритм EID_EASE_OUT_CUBIC (3) для плавной доводки
        self.anim_scale = animCreate(200, 3) 
        animTo(self.anim_scale, 1.0)
    end

    return self
end

function Window:draw(mx, my, mdown, dt)
    if not self.active then return end

    local sw, sh = getScreenSize()
    
    -- Просчитываем текущий шаг анимации появления
    local scale = 1.0
    if self.anim_scale then
        animStep(self.anim_scale, dt)
        scale = animEval(self.anim_scale)
    end

    -- Рассчитываем анимированные координаты (плавный зум из геометрического центра окна)
    local cx = self.x + self.w / 2
    local cy = self.y + self.h / 2
    local win_w = math.floor(self.w * scale)
    local win_h = math.floor(self.h * scale)
    local win_x = math.floor(cx - win_w / 2)
    local win_y = math.floor(cy - win_h / 2)

    if self.fullscreen then
        win_x, win_y = 0, 0
        win_w, win_h = sw, sh
    end

    local active = (focused_window == self)
    
    -- Рендеринг рамок окна
    if not self.borderless and not self.fullscreen then
        local ty = win_y - 28
        
        -- 1. Симуляция мягкой тени вокруг окна (для эффекта глубины)
        drawRect(win_x - 3, ty - 3, win_w + 6, win_h + 34, 0x0A0B0D) -- Внешний темный ореол
        drawRect(win_x - 1, ty - 1, win_w + 2, win_h + 30, 0x14161C) -- Плотная тень границы

        -- 2. Обводка активного/неактивного окна (световой контур)
        local border_color = active and 0x0078D7 or 0x2A2E3D
        drawRect(win_x - 1, ty - 1, win_w + 2, win_h + 30, border_color)

        -- 3. Эффект размытия Acrylic / Aero Glass для заголовка окна
        if type(drawBlur) == "function" then
            -- Размываем то, что под заголовком, и слегка затеняем (активное окно ярче)
            drawBlur(win_x, ty, win_w, 28, active and 0.55 or 0.45)
        else
            drawRect(win_x, ty, win_w, 28, active and 0x1A72BB or 0x2D2D30)
        end

        -- Тонкий световой блик на верхней грани активного окна (как в macOS/Windows)
        if active then
            drawRect(win_x, ty, win_w, 1, 0x55AFFF)
        end

        -- Текст заголовка
        drawText(self.title, win_x + 10, ty + 8, 0xFFFFFF)

        -- Кнопка закрытия окна "X"
        local bx = win_x + win_w - 26
        if button("X", bx, ty + 4, 22, 20) then
            self.active = false
            return
        end

        -- Тонкая разделительная черта под заголовком
        drawRect(win_x, win_y - 1, win_w, 1, active and 0x0078D7 or 0x333333)
    end

    -- Рендеринг тела окна
    if not self.is_app_container then
        if not self.borderless then
            -- Эффект Acrylic Glass для тела окна! 
            -- Размываем фоновый рабочий стол под окном и плавно затеняем его до 60% яркости
            if type(drawBlur) == "function" then
                drawBlur(win_x, win_y, win_w, win_h, 0.60)
            else
                drawRect(win_x, win_y, win_w, win_h, 0x1E1E1E)
            end
        end
        
        -- Вызов отрисовки контента приложения
        if self.draw_cb then 
            -- Подменяем координаты на анимированные, чтобы контент плавно масштабировался вместе с окном
            local original_x, original_y = self.x, self.y
            local original_w, original_h = self.w, self.h
            self.x, self.y = win_x, win_y
            self.w, self.h = win_w, win_h

            self.draw_cb(self, mx, my, mdown, dt) 

            self.x, self.y = original_x, original_y
            self.w, self.h = original_w, original_h
        end
    else
        -- Контейнер для внешних приложений (Doom/Snake)
        if type(setAppWindowPos) == "function" then
            setAppWindowPos(win_x, win_y, win_w, win_h)
        end
    end

    -- Точка изменения размера (Resize Handle) в правом нижнем углу
    -- Появляется только когда окно полностью раскрылось
    if not self.borderless and not self.fullscreen and scale >= 0.95 then
        local rx, ry = win_x + win_w - 10, win_y + win_h - 10
        drawRect(rx, ry, 10, 10, active and 0x0078D7 or 0x444444)
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