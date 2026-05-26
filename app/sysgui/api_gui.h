#ifndef API_GUI_H
#define API_GUI_H

#include "lua/lua.h"

// Объявляем функцию, чтобы main.c её видел
void register_gui_api(lua_State *L);

#endif