#include "stm32f4xx_hal.h"
#include "lua.h"
#include "lauxlib.h"
#include "lua_hal.h"

int l_rd(lua_State* L)
{
	lua_Integer addr = luaL_checkinteger(L, 1);
	lua_pushinteger(L, *(uint32_t*)(addr));
	return 1;
}

int l_wr(lua_State* L)
{
	lua_Integer addr = luaL_checkinteger(L, 1);
	lua_Integer val = luaL_checkinteger(L, 2);
	*(uint32_t*)(addr) = val;
	return 0;
}

int l_setbits(lua_State* L)
{
	lua_Integer addr = luaL_checkinteger(L, 1);
	lua_Integer mask = luaL_checkinteger(L, 2);
	*(uint32_t*)(addr) = *(uint32_t*)(addr) | mask;
	return 0;
}

int l_clrbits(lua_State* L)
{
	lua_Integer addr = luaL_checkinteger(L, 1);
	lua_Integer mask = luaL_checkinteger(L, 2);
	*(uint32_t*)(addr) = *(uint32_t*)(addr) & ~mask;
	return 0;
}

GPIO_TypeDef* ports[] = {GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG, GPIOH};

int l_dr(lua_State* L)
{
	const char  *port = luaL_checkstring (L, 1);
    lua_Integer  n    = luaL_checkinteger(L, 2);
    GPIO_PinState s = HAL_GPIO_ReadPin(ports[port[0]-'A'], n);
    lua_pushinteger(L, s == GPIO_PIN_SET ? 1 : 0);
    return 1;
}

int l_dw(lua_State* L)
{
	const char  *port = luaL_checkstring (L, 1);
    lua_Integer  n = luaL_checkinteger(L, 2);
    lua_Integer  val = luaL_checkinteger(L, 3);
    HAL_GPIO_WritePin(ports[port[0]-'A'], n, val ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return 0;
}