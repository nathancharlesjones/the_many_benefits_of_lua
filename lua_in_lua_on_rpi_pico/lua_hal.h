#ifndef LUA_HAL_H
#define LUA_HAL_H

#include "lua.h"

/**
 * Register all HAL binding functions as globals in the given Lua state.
 *
 * Call this once after luaL_newstate() and library loading, before entering
 * the REPL loop.  Every function lands in the global namespace so you can
 * type  rd(0x40020000)  directly at the prompt.
 *
 * Functions registered:
 *   rd(addr, [n=1], [width=32])         -- read n memory-mapped registers
 *   wr(addr, val, [width=32])           -- write one register
 *   setbits(addr, mask)                 -- RMW: OR-in mask
 *   clrbits(addr, mask)                 -- RMW: AND-out mask
 *   pin(port, n, mode, [pull], [af])    -- configure GPIO pin
 *   dw(port, n, val)                    -- digital write (0 or 1)
 *   dr(port, n)                         -- digital read  (returns 0 or 1)
 *   help([cmd])                         -- human-readable usage
 *   cmds()                              -- machine-parseable command list
 *   ver()                               -- firmware / protocol version
 *   reset([mode])                       -- soft-reset; mode = "boot"|"dfu"
 */
void luahal_register(lua_State *L);

#endif /* LUA_HAL_H */
