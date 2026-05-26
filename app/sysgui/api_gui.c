#include "api_gui.h"
#include "lua/lauxlib.h"
#include "lua/lua.h"
#include "lua/lualib.h"
#include <eid.h>
#include <eid_ext.h>
#include <equos.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

extern uint32_t *draw_target;
extern uint32_t screen_w, screen_h;
extern eid_ctx_t eid_ctx;

#define MAX_ANIMS 32
static eid_anim_t anims[MAX_ANIMS];
static int anim_count = 0;

bool is_any_anim_active(void) {
  for (int i = 0; i < anim_count; i++) {
    if (anims[i].active) {
      return true;
    }
  }
  return false;
}

static int l_anim_create(lua_State *L) {
  float duration = (float)luaL_checknumber(L, 1);
  int ease = luaL_checkinteger(L, 2);

  if (anim_count >= MAX_ANIMS) {
    lua_pushinteger(L, -1);
    return 1;
  }

  int id = anim_count++;
  eid_anim_init(&anims[id], duration, (eid_ease_t)ease);
  lua_pushinteger(L, id);
  return 1;
}

static int l_anim_to(lua_State *L) {
  int id = luaL_checkinteger(L, 1);
  float target = (float)luaL_checknumber(L, 2);
  if (id >= 0 && id < anim_count) {
    eid_anim_to(&anims[id], target);
  }
  return 0;
}

static int l_anim_step(lua_State *L) {
  int id = luaL_checkinteger(L, 1);
  float dt = (float)luaL_checknumber(L, 2);
  if (id >= 0 && id < anim_count) {
    eid_anim_step(&anims[id], dt);
  }
  return 0;
}

static int l_anim_eval(lua_State *L) {
  int id = luaL_checkinteger(L, 1);
  if (id >= 0 && id < anim_count) {
    lua_pushnumber(L, eid_anim_eval(&anims[id]));
  } else {
    lua_pushnumber(L, 0.0f);
  }
  return 1;
}

static int l_draw_text(lua_State *L) {
  const char *str = luaL_checkstring(L, 1);
  int x = luaL_checkinteger(L, 2);
  int y = luaL_checkinteger(L, 3);
  uint32_t color = (uint32_t)luaL_checknumber(L, 4);

  eid_draw_text(draw_target, screen_w, screen_h, x, y, str, color);
  return 0;
}

static int l_draw_rect(lua_State *L) {
  int x = luaL_checkinteger(L, 1);
  int y = luaL_checkinteger(L, 2);
  int w = luaL_checkinteger(L, 3);
  int h = luaL_checkinteger(L, 4);
  uint32_t color = (uint32_t)luaL_checknumber(L, 5);

  eid_draw_rect(draw_target, screen_w, screen_h, x, y, w, h, color);
  return 0;
}

static int l_draw_gradient(lua_State *L) {
  int x = luaL_checkinteger(L, 1);
  int y = luaL_checkinteger(L, 2);
  int w = luaL_checkinteger(L, 3);
  int h = luaL_checkinteger(L, 4);
  uint32_t c1 = (uint32_t)luaL_checknumber(L, 5);
  uint32_t c2 = (uint32_t)luaL_checknumber(L, 6);
  bool vertical = lua_toboolean(L, 7);

  eid_draw_gradient_rect(draw_target, screen_w, screen_h, x, y, w, h, c1, c2,
                         vertical);
  return 0;
}

static int l_draw_line(lua_State *L) {
  int x1 = luaL_checkinteger(L, 1);
  int y1 = luaL_checkinteger(L, 2);
  int x2 = luaL_checkinteger(L, 3);
  int y2 = luaL_checkinteger(L, 4);
  uint32_t color = (uint32_t)luaL_checknumber(L, 5);

  eid_draw_line(draw_target, screen_w, screen_h, x1, y1, x2, y2, color);
  return 0;
}

static int l_button(lua_State *L) {
  const char *label = luaL_checkstring(L, 1);
  int x = luaL_checkinteger(L, 2);
  int y = luaL_checkinteger(L, 3);
  int w = luaL_checkinteger(L, 4);
  int h = luaL_checkinteger(L, 5);

  uint32_t state = eid_button(&eid_ctx, label, x, y, w, h);
  lua_pushboolean(L, (state & EID_STATE_CLICKED) != 0);
  return 1;
}

static int l_checkbox(lua_State *L) {
  const char *label = luaL_checkstring(L, 1);
  int x = luaL_checkinteger(L, 2);
  int y = luaL_checkinteger(L, 3);
  bool val = lua_toboolean(L, 4);

  eid_checkbox(&eid_ctx, label, x, y, &val);
  lua_pushboolean(L, val);
  return 1;
}

static int l_slider(lua_State *L) {
  const char *label = luaL_checkstring(L, 1);
  int x = luaL_checkinteger(L, 2);
  int y = luaL_checkinteger(L, 3);
  int w = luaL_checkinteger(L, 4);
  float val = (float)luaL_checknumber(L, 5);
  float min = (float)luaL_checknumber(L, 6);
  float max = (float)luaL_checknumber(L, 7);

  eid_slider(&eid_ctx, label, x, y, w, &val, min, max);
  lua_pushnumber(L, val);
  return 1;
}

static int l_exec(lua_State *L) {
  const char *cmd = luaL_checkstring(L, 1);
  int ret = sys_exec(cmd);
  lua_pushinteger(L, ret);
  return 1;
}

static int l_get_uptime(lua_State *L) {
  uint32_t ms = (uint32_t)_syscall(SYS_GET_TIME, 0, 0, 0, 0, 0);
  lua_pushnumber(L, (double)ms / 1000.0);
  return 1;
}

static int l_get_mem_info(lua_State *L) {
  uint64_t used = sys_get_used_mem();
  uint64_t total = sys_get_total_mem();
  lua_pushnumber(L, (double)used);
  lua_pushnumber(L, (double)total);
  return 2;
}

static int l_get_mouse(lua_State *L) {
  lua_pushinteger(L, eid_ctx.mx);
  lua_pushinteger(L, eid_ctx.my);
  lua_pushboolean(L, eid_ctx.m_down);
  return 3;
}

static int l_get_last_key(lua_State *L) {
  lua_pushinteger(L, eid_ctx.last_key);
  eid_ctx.last_key = 0; // Сбрасываем сканкод
  return 1;
}

static int l_scancode_to_ascii(lua_State *L) {
  int sc = luaL_checkinteger(L, 1);
  bool shift = lua_toboolean(L, 2);
  char c = eid_scancode_to_ascii((uint8_t)sc, shift);
  char str[2] = {c, 0};
  lua_pushstring(L, str);
  return 1;
}

static int l_read_file(lua_State *L) {
  const char *filename = luaL_checkstring(L, 1);
  uint32_t size = 0;
  uint64_t addr =
      _syscall(SYS_READ_FILE, (uint64_t)filename, (uint64_t)&size, 0, 0, 0);
  if (addr && size > 0) {
    lua_pushlstring(L, (const char *)addr, size);
  } else {
    lua_pushnil(L);
  }
  return 1;
}

static int l_save_file(lua_State *L) {
  const char *filename = luaL_checkstring(L, 1);
  size_t len = 0;
  /* Lua strings are length-prefixed and may contain embedded NUL bytes
   * (e.g. binary blobs from readFile). Using strlen() here truncated any
   * payload at the first 0x00 — silently corrupting binary saves. */
  const char *data = luaL_checklstring(L, 2, &len);
  write_file(filename, (void *)data, (int)len);
  return 0;
}

static int l_get_files(lua_State *L) {
  lua_newtable(L);
  int idx = 1;

  // Локальный буфер в пространстве пользователя
  struct {
    char name[128];
    uint32_t size;
    char dev[32];
  } entry;

  for (int i = 0;; i++) {
    // Делаем системный вызов SYS_READ_DIR
    // Передаем индекс файла `i` и адрес буфера `entry`
    uint64_t ret = _syscall(SYS_READ_DIR, i, (uint64_t)&entry, 0, 0, 0);
    if (!ret) {
      break; // Если ядро вернуло 0, значит, файлы закончились
    }

    lua_newtable(L);

    lua_pushstring(L, "name");
    lua_pushstring(L, entry.name);
    lua_settable(L, -3);

    lua_pushstring(L, "size");
    lua_pushinteger(L, entry.size);
    lua_settable(L, -3);

    lua_pushstring(L, "dev");
    lua_pushstring(L, entry.dev);
    lua_settable(L, -3);

    lua_rawseti(L, -2, idx++);
  }
  return 1;
}

static int l_get_screen_size(lua_State *L) {
  lua_pushinteger(L, screen_w);
  lua_pushinteger(L, screen_h);
  return 2; // Возвращаем два значения
}

// getTasks() -> { {pid=, state="RUNNING"|"STOPPED", cr3=, brk=}, ... }
static int l_get_tasks(lua_State *L) {
  lua_newtable(L);
  int out_idx = 1;
  sys_task_info_t info;
  for (int i = 0; i < 256; i++) {
    uint64_t ok = _syscall(SYS_TASK_INFO, (uint64_t)i, (uint64_t)&info, 0, 0, 0);
    if (!ok) break;
    lua_newtable(L);
    lua_pushstring(L, "pid");
    lua_pushinteger(L, (lua_Integer)info.pid);
    lua_settable(L, -3);
    lua_pushstring(L, "state");
    lua_pushstring(L, info.running ? "RUNNING" : "STOPPED");
    lua_settable(L, -3);
    lua_pushstring(L, "cr3");
    lua_pushinteger(L, (lua_Integer)info.cr3);
    lua_settable(L, -3);
    lua_pushstring(L, "brk");
    lua_pushinteger(L, (lua_Integer)info.brk);
    lua_settable(L, -3);
    lua_rawseti(L, -2, out_idx++);
  }
  return 1;
}

// killTask(pid) -> bool
static int l_kill_task(lua_State *L) {
  lua_Integer pid = luaL_checkinteger(L, 1);
  uint64_t ok = _syscall(SYS_TASK_KILL, (uint64_t)pid, 0, 0, 0, 0);
  lua_pushboolean(L, ok ? 1 : 0);
  return 1;
}

// killAllTasks() -> integer (kill count)
static int l_kill_all_tasks(lua_State *L) {
  uint64_t n = _syscall(SYS_TASK_KILLALL, 0, 0, 0, 0, 0);
  lua_pushinteger(L, (lua_Integer)n);
  return 1;
}

// shellExec(line) -> string
//
// Прокидываем команду в ring-0 шелл (src/system/shell/shell.c, см.
// shellsyntx.h). Вывод собирается во временный sink и возвращается как
// одна строка Lua. Это позволяет Lua-терминалу в init.lua не дублировать
// у себя реестр команд: всё, что умеет kernel-shell (help/ps/kill/
// killall/run/...), автоматически доступно отсюда.
//
// Длина результата ограничена 2 КБ (см. SYS_SHELL_EXEC). Если строка
// длиннее — она усекается, NUL-терминация гарантирована.
static int l_shell_exec(lua_State *L) {
  const char *line = luaL_checkstring(L, 1);
  static char outbuf[2048];
  outbuf[0] = '\0';
  uint64_t n = _syscall(SYS_SHELL_EXEC,
                        (uint64_t)line,
                        (uint64_t)outbuf,
                        (uint64_t)sizeof(outbuf),
                        0, 0);
  if (n >= sizeof(outbuf)) n = sizeof(outbuf) - 1;
  outbuf[n] = '\0';
  lua_pushlstring(L, outbuf, (size_t)n);
  return 1;
}

void register_gui_api(lua_State *L) {
  lua_register(L, "drawText", l_draw_text);
  lua_register(L, "drawRect", l_draw_rect);
  lua_register(L, "drawGradient", l_draw_gradient);
  lua_register(L, "drawLine", l_draw_line);

  lua_register(L, "animCreate", l_anim_create);
  lua_register(L, "animTo", l_anim_to);
  lua_register(L, "animStep", l_anim_step);
  lua_register(L, "animEval", l_anim_eval);

  lua_register(L, "button", l_button);
  lua_register(L, "checkbox", l_checkbox);
  lua_register(L, "slider", l_slider);

  lua_register(L, "exec", l_exec);
  lua_register(L, "getUptime", l_get_uptime);
  lua_register(L, "getMemInfo", l_get_mem_info);
  lua_register(L, "getMouse", l_get_mouse);
  lua_register(L, "getLastKey", l_get_last_key);
  lua_register(L, "scancodeToAscii", l_scancode_to_ascii);
  lua_register(L, "readFile", l_read_file);
  lua_register(L, "saveFile", l_save_file);
  lua_register(L, "getFiles", l_get_files);
  lua_register(L, "getScreenSize", l_get_screen_size);

  /* ps / kill / killall bridges for the Lua ring-3 terminal */
  lua_register(L, "getTasks",      l_get_tasks);
  lua_register(L, "killTask",      l_kill_task);
  lua_register(L, "killAllTasks",  l_kill_all_tasks);

  /* one-shot ring-0 shell bridge — Lua terminal delegates everything here */
  lua_register(L, "shellExec",     l_shell_exec);
}