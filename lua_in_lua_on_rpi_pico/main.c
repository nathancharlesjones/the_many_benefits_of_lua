//#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "lua_hal.h" 
#include "embedded_cli.h"
#include "lua_hal.h"

static lua_State *L;
static struct embedded_cli cli;

static void put_char(void *data, char ch, bool is_last)
{
    (void)data; (void)is_last;
    putchar(ch);
}

int l_cli_prompt(lua_State* L)
{
  embedded_cli_prompt(&cli);
  return 0;
}

int l_get_line(lua_State* L)
{
  if (embedded_cli_insert_char(&cli, getchar()))
  {
      const char *line = embedded_cli_get_line(&cli);
      if (line && strlen(line) > 0)
      {
          lua_pushstring(L, line);
          return 1;
      }
  }
  return 0;
}

int main() {
    stdio_init_all();

    L = luaL_newstate();
    luaL_requiref(L, "_G",     luaopen_base,   1); lua_pop(L, 1);
    luaL_requiref(L, "math",   luaopen_math,   1); lua_pop(L, 1);
    luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, "table",  luaopen_table,  1); lua_pop(L, 1);
  
    lua_register(L, "cli_prompt", l_cli_prompt);
    lua_register(L, "get_line", l_get_line);
    luahal_register(L);  // Registers rd, wr, pin, dr, dw, etc
  
    embedded_cli_init(&cli, "> ", put_char, NULL);
    printf("Finished setup\n\r");
  
    const char* repl = 
        "function repl()\n"
        "  cli_prompt()\n"
        "  while true do\n"
        "    local line = get_line()\n"
        "    if line then\n"
        "      local fn, err = load(line)\n"
        "      if fn then pcall(fn) else print(err) end"
        "      cli_prompt()\n"
        "    end\n"
        "  end\n"
        "end\n"
        "repl()\n";
    
    luaL_dostring(L, repl); // Never returns
}