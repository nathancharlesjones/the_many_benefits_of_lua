/* lua_hal_STM32F4.c
 *
 * STM32F4 HAL back-end for the Lua binding layer.
 *
 * Drop-in replacement for lua_hal.c (the stub).  Compile exactly one of the
 * two into your project; both export the same luahal_register() symbol.
 *
 * Extra globals registered beyond the base command set:
 *   ticks()     — HAL_GetTick() in milliseconds since reset
 *   sleep(ms)   — HAL_Delay(ms); polls for Ctrl-C (0x03) so loops can be
 *                 interrupted from the terminal
 *   hex(n)      — format an integer as "0x%08X"; handy for register values
 *
 * Register-address globals (see luahal_register_regs):
 *   GPIOx_MODER, GPIOx_OTYPER, GPIOx_OSPEEDR, GPIOx_PUPDR,
 *   GPIOx_IDR, GPIOx_ODR, GPIOx_BSRR, GPIOx_LCKR,
 *   GPIOx_AFRL, GPIOx_AFRH   (x = A..H)
 *   RCC_AHB1ENR, RCC_APB1ENR, RCC_APB2ENR, RCC_CR, RCC_CFGR
 *
 * These are plain Lua integers (the register's byte address), so
 *   rd(GPIOA_IDR)            -- read PA input level word
 *   rd(GPIOA_IDR) & (1<<5)   -- test PA5 (Lua 5.3+ bitwise syntax)
 *   hex(rd(GPIOC_IDR))       -- pretty-print
 * all work without any special parsing in C.
 */

#include <stdio.h>
#include <string.h>

#include "stm32f4xx_hal.h"
#include "lua.h"
#include "lauxlib.h"
#include "lua_hal.h"

/* The REPL console UART, needed by l_sleep() to poll for Ctrl-C.
 * Defined in main.c by CubeMX; extern avoids touching that file. */
extern UART_HandleTypeDef huart2;

/* ============================================================
 *  Internal helpers
 * ============================================================ */

/** Map a port letter string ("A"–"H", case-insensitive) to its GPIO_TypeDef*.
 *  Raises a Lua error on bad input; never returns NULL. */
static GPIO_TypeDef *port_to_gpio(lua_State *L, const char *port)
{
    if (port[1] == '\0') {
        switch (port[0] | 0x20) {          /* fold to lower-case */
            case 'a': return GPIOA;
            case 'b': return GPIOB;
            case 'c': return GPIOC;
            case 'd': return GPIOD;
            case 'e': return GPIOE;
#ifdef GPIOF
            case 'f': return GPIOF;
#endif
#ifdef GPIOG
            case 'g': return GPIOG;
#endif
            case 'h': return GPIOH;
        }
    }
    luaL_error(L, "unknown GPIO port '%s'", port);
    return NULL;    /* unreachable; suppresses compiler warning */
}

/** Convert pin number 0-15 to a HAL GPIO_PIN_x bitmask.
 *  Raises a Lua error on out-of-range input. */
static uint16_t n_to_pin(lua_State *L, lua_Integer n)
{
    luaL_argcheck(L, n >= 0 && n <= 15, 2, "pin number must be 0-15");
    return (uint16_t)(1u << (unsigned)n);
}

/** Enable the AHB1 clock for a GPIO port.  Idempotent. */
static void gpio_clk_enable(GPIO_TypeDef *gpio)
{
    if      (gpio == GPIOA) { __HAL_RCC_GPIOA_CLK_ENABLE(); }
    else if (gpio == GPIOB) { __HAL_RCC_GPIOB_CLK_ENABLE(); }
    else if (gpio == GPIOC) { __HAL_RCC_GPIOC_CLK_ENABLE(); }
    else if (gpio == GPIOD) { __HAL_RCC_GPIOD_CLK_ENABLE(); }
    else if (gpio == GPIOE) { __HAL_RCC_GPIOE_CLK_ENABLE(); }
#ifdef GPIOF
    else if (gpio == GPIOF) { __HAL_RCC_GPIOF_CLK_ENABLE(); }
#endif
#ifdef GPIOG
    else if (gpio == GPIOG) { __HAL_RCC_GPIOG_CLK_ENABLE(); }
#endif
    else if (gpio == GPIOH) { __HAL_RCC_GPIOH_CLK_ENABLE(); }
}

static uint32_t mode_to_hal(const char *mode)
{
    if (!strcmp(mode, "in"))     return GPIO_MODE_INPUT;
    if (!strcmp(mode, "out"))    return GPIO_MODE_OUTPUT_PP;
    if (!strcmp(mode, "af"))     return GPIO_MODE_AF_PP;
    if (!strcmp(mode, "analog")) return GPIO_MODE_ANALOG;
    return 0;   /* already validated upstream */
}

static uint32_t pull_to_hal(const char *pull)
{
    if (!strcmp(pull, "none")) return GPIO_NOPULL;
    if (!strcmp(pull, "up"))   return GPIO_PULLUP;
    if (!strcmp(pull, "down")) return GPIO_PULLDOWN;
    return 0;
}

/* ============================================================
 *  Raw register access
 * ============================================================ */

/*
 * rd(addr, [n=1], [width=32])
 *
 * Read n consecutive memory-mapped locations of the given bit-width starting
 * at addr.  Returns n integers on the Lua stack.
 *
 *   val              = rd(0x40020014)
 *   val              = rd(GPIOA_IDR)        -- using named constant
 *   b0, b1, b2       = rd(0x20000000, 3, 8)
 *   hex(rd(GPIOC_IDR))                      -- pretty-print
 */
static int l_rd(lua_State *L)
{
    lua_Integer addr  = luaL_checkinteger(L, 1);
    lua_Integer n     = luaL_optinteger (L, 2, 1);
    lua_Integer width = luaL_optinteger (L, 3, 32);

    luaL_argcheck(L, width == 8 || width == 16 || width == 32, 3, "width must be 8, 16, or 32");
    luaL_argcheck(L, n >= 1, 2, "n must be >= 1");

    for (lua_Integer i = 0; i < n; i++) {
        uintptr_t a = (uintptr_t)(addr + i * (width / 8));
        lua_Integer val;
        switch (width) {
            case  8: val = (lua_Integer)*(volatile uint8_t  *)a; break;
            case 16: val = (lua_Integer)*(volatile uint16_t *)a; break;
            default: val = (lua_Integer)*(volatile uint32_t *)a; break;
        }
        lua_pushinteger(L, val);
    }
    return (int)n;
}

/*
 * wr(addr, val, [width=32])
 *
 *   wr(0x40020014, 0x00000020)
 *   wr(GPIOA_ODR, 0x0020)
 */
static int l_wr(lua_State *L)
{
    lua_Integer addr  = luaL_checkinteger(L, 1);
    lua_Integer val   = luaL_checkinteger(L, 2);
    lua_Integer width = luaL_optinteger (L, 3, 32);

    luaL_argcheck(L, width == 8 || width == 16 || width == 32, 3, "width must be 8, 16, or 32");

    uintptr_t a = (uintptr_t)addr;
    switch (width) {
        case  8: *(volatile uint8_t  *)a = (uint8_t) val; break;
        case 16: *(volatile uint16_t *)a = (uint16_t)val; break;
        default: *(volatile uint32_t *)a = (uint32_t)val; break;
    }
    return 0;
}

/*
 * setbits(addr, mask)
 *
 * Read-modify-write: OR mask into the 32-bit register at addr.
 *
 *   setbits(RCC_AHB1ENR, 0x04)    -- enable GPIOC clock (bit 2)
 */
static int l_setbits(lua_State *L)
{
    lua_Integer addr = luaL_checkinteger(L, 1);
    lua_Integer mask = luaL_checkinteger(L, 2);
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)addr;
    *p |= (uint32_t)mask;
    return 0;
}

/*
 * clrbits(addr, mask)
 *
 * Read-modify-write: AND complement of mask into the 32-bit register at addr.
 *
 *   clrbits(GPIOA_MODER, 0x0C00)  -- clear mode bits for PA5
 */
static int l_clrbits(lua_State *L)
{
    lua_Integer addr = luaL_checkinteger(L, 1);
    lua_Integer mask = luaL_checkinteger(L, 2);
    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)addr;
    *p &= ~(uint32_t)mask;
    return 0;
}

/* ============================================================
 *  GPIO
 * ============================================================ */

/*
 * pin(port, n, mode, [pull="none"], [af])
 *
 * Configures a pin via HAL_GPIO_Init.  Automatically enables the GPIO clock.
 *   port  "A".."H"
 *   n     0-15
 *   mode  "in" | "out" | "af" | "analog"
 *   pull  "none" | "up" | "down"
 *   af    0-15  (required when mode == "af")
 *
 *   pin("A", 5, "out")              -- PA5 push-pull output (LD2 on Nucleo)
 *   pin("C", 13, "in", "up")        -- PC13 input pull-up (B1 on Nucleo)
 *   pin("B", 6, "af", "none", 7)    -- PB6 AF7 = USART1_TX on F4
 */
static int l_pin(lua_State *L)
{
    static const char *mode_opts[] = {"in", "out", "af", "analog", NULL};
    static const char *pull_opts[] = {"none", "up", "down", NULL};

    const char  *port = luaL_checkstring (L, 1);
    lua_Integer  n    = luaL_checkinteger(L, 2);
    int          mode = luaL_checkoption (L, 3, NULL,   mode_opts);
    int          pull = luaL_checkoption (L, 4, "none", pull_opts);
    lua_Integer  af   = luaL_optinteger  (L, 5, -1);

    /* mode index 2 == "af" */
    luaL_argcheck(L, mode != 2 || af >= 0,  5, "af number required when mode is 'af'");
    luaL_argcheck(L, mode != 2 || af <= 15, 5, "af number must be 0-15");

    GPIO_TypeDef *gpio = port_to_gpio(L, port);
    gpio_clk_enable(gpio);

    GPIO_InitTypeDef init = {
        .Pin       = n_to_pin(L, n),
        .Mode      = mode_to_hal(mode_opts[mode]),
        .Pull      = pull_to_hal(pull_opts[pull]),
        .Speed     = GPIO_SPEED_FREQ_LOW,
        .Alternate = (uint32_t)(af >= 0 ? af : 0),
    };
    HAL_GPIO_Init(gpio, &init);
    return 0;
}

/*
 * dw(port, n, val)
 *
 * Drive pin high (1) or low (0) via HAL_GPIO_WritePin.
 *
 *   dw("A", 5, 1)    -- PA5 high → LD2 on
 */
static int l_dw(lua_State *L)
{
    const char  *port = luaL_checkstring (L, 1);
    lua_Integer  n    = luaL_checkinteger(L, 2);
    lua_Integer  val  = luaL_checkinteger(L, 3);

    luaL_argcheck(L, val == 0 || val == 1, 3, "val must be 0 or 1");

    GPIO_TypeDef *gpio = port_to_gpio(L, port);
    HAL_GPIO_WritePin(gpio, n_to_pin(L, n),
                      val ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return 0;
}

/*
 * dr(port, n)   →  0 or 1
 *
 * Sample the input level via HAL_GPIO_ReadPin.
 *
 *   dr("C", 13)      -- 1 = not pressed, 0 = pressed (B1 is active-low)
 */
static int l_dr(lua_State *L)
{
    const char  *port = luaL_checkstring (L, 1);
    lua_Integer  n    = luaL_checkinteger(L, 2);

    GPIO_TypeDef *gpio = port_to_gpio(L, port);
    GPIO_PinState s = HAL_GPIO_ReadPin(gpio, n_to_pin(L, n));
    lua_pushinteger(L, s == GPIO_PIN_SET ? 1 : 0);
    return 1;
}

/* ============================================================
 *  Timing utilities (STM32F4 additions)
 * ============================================================ */

/*
 * ticks()   →  integer
 *
 * Returns HAL_GetTick() — milliseconds elapsed since reset.
 * Use for deadline-based loops that stay interruptible:
 *
 *   local t0 = ticks()
 *   while ticks()-t0 < 5000 do dw("A",5,1) sleep(250) dw("A",5,0) sleep(250) end
 */
static int l_ticks(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)HAL_GetTick());
    return 1;
}

/*
 * sleep(ms)
 *
 * Block for ms milliseconds using HAL_Delay, but poll the console UART on
 * each tick.  Receiving Ctrl-C (0x03) raises a Lua "interrupted" error which
 * the REPL prints and recovers from normally.
 *
 * Rule: any Lua loop that should be breakable must call sleep() at least
 * once per iteration — even sleep(1) is enough.
 *
 * Note: bytes arriving during sleep (other than Ctrl-C) are consumed and
 * discarded.  This drains the UART receive register and prevents overrun,
 * at the cost of losing keystrokes typed mid-loop.
 */
static int l_sleep(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms < 0) ms = 0;

    uint32_t start = HAL_GetTick();
    while ((HAL_GetTick() - start) < (uint32_t)ms) {
        uint8_t ch = 0;
        if (HAL_UART_Receive(&huart2, &ch, 1, 0) == HAL_OK && ch == 0x03)
            return luaL_error(L, "interrupted");
        HAL_Delay(1);
    }
    return 0;
}

/* ============================================================
 *  Formatting utility (STM32F4 addition)
 * ============================================================ */

/*
 * hex(n)   →  string "0xXXXXXXXX"
 *
 * The REPL prints integers in decimal by default, which is unpleasant for
 * register values.  Wrap any expression in hex() at the prompt:
 *
 *   hex(rd(GPIOA_IDR))      -->  "0x00000020"
 *   hex(rd(RCC_AHB1ENR))    -->  "0x000000FF"
 */
static int l_hex(lua_State *L)
{
    lua_Integer v = luaL_checkinteger(L, 1);
    char buf[11];
    snprintf(buf, sizeof(buf), "0x%08X", (unsigned)v);
    lua_pushstring(L, buf);
    return 1;
}

/* ============================================================
 *  Introspection
 * ============================================================ */

static int l_help(lua_State *L)
{
    const char *cmd = luaL_optstring(L, 1, NULL);
    if (cmd) {
        printf("help: no detailed entry for '%s' — see lua_hal_STM32F4.c\r\n", cmd);
        return 0;
    }
    printf(
        "HAL commands (STM32F4)\r\n"
        "  rd(addr,[n],[w])              read n regs, width w=8/16/32\r\n"
        "  wr(addr,val,[w])              write register\r\n"
        "  setbits(addr,mask)            RMW: OR in mask\r\n"
        "  clrbits(addr,mask)            RMW: AND out mask\r\n"
        "  pin(port,n,mode,[pull],[af])  configure GPIO pin\r\n"
        "  dw(port,n,val)               digital write (0 or 1)\r\n"
        "  dr(port,n)                   digital read  -> 0 or 1\r\n"
        "  ticks()                      ms since reset\r\n"
        "  sleep(ms)                    delay; Ctrl-C interrupts loop\r\n"
        "  hex(n)                       format integer as 0xXXXXXXXX\r\n"
        "  help([cmd])                  this message\r\n"
        "  cmds()                       machine-parseable list\r\n"
        "  ver()                        firmware version\r\n"
        "  reset([mode])               reset; mode=boot|dfu\r\n"
        "Register constants: GPIOx_IDR, GPIOx_ODR, ... (x=A..H)\r\n"
        "                    RCC_AHB1ENR, RCC_APB1ENR, RCC_APB2ENR\r\n"
    );
    return 0;
}

static int l_cmds(lua_State *L)
{
    (void)L;
    printf("rd,wr,setbits,clrbits,pin,dw,dr,ticks,sleep,hex,help,cmds,ver,reset\r\n");
    return 0;
}

static int l_ver(lua_State *L)
{
    (void)L;
    printf("fw=0.1.0 proto=1 target=STM32F4 build=" __DATE__ " " __TIME__ "\r\n");
    return 0;
}

/* ============================================================
 *  Control
 * ============================================================ */

/*
 * Jump to the STM32F4 system-memory bootloader (USART/USB DFU).
 * System memory on all STM32F4 parts is at 0x1FFF_0000.
 */
static void jump_to_system_memory(void)
{
    /* Orderly teardown: give UART time to drain, then go quiet */
    HAL_Delay(10);
    __disable_irq();
    HAL_RCC_DeInit();
    HAL_DeInit();

    /* Stop SysTick so it doesn't fire inside the bootloader */
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL  = 0;

    const uint32_t SYSMEM = 0x1FFF0000UL;
    __set_MSP(*(volatile uint32_t *) SYSMEM);
    ((void (*)(void))(*(volatile uint32_t *)(SYSMEM + 4u)))();
    /* never returns */
}

/*
 * reset([mode])
 *
 *   reset()         NVIC_SystemReset — plain reboot
 *   reset("boot")   jump to system-memory bootloader (USART/SPI/I2C/USB DFU)
 *   reset("dfu")    same — both aliases jump to the same ST bootloader
 */
static int l_reset(lua_State *L)
{
    static const char *reset_opts[] = {"reset", "boot", "dfu", NULL};
    int mode = luaL_checkoption(L, 1, "reset", reset_opts);

    if (mode > 0)   /* "boot" or "dfu" — both jump to ST system-memory bootloader */
        jump_to_system_memory();
    else {
        HAL_Delay(10);
        NVIC_SystemReset();
    }
    return 0;   /* unreachable */
}

/* ============================================================
 *  Register-address constants → Lua globals
 * ============================================================
 *
 * LUA_REG(L, periph, field) takes a peripheral pointer and a struct field
 * and pushes the field's byte address as a Lua global.  The global name is
 * formed by stringifying both tokens:
 *
 *   LUA_REG(L, GPIOA, IDR)   →   GPIOA_IDR = 0x40020010
 *
 * Because the address comes from &periph->field (the actual C struct), it is
 * always correct for the target part — no hand-coding, no risk of typos.
 *
 * Adjacent C string literals concatenate at compile time, so
 *   #periph "_" #field   =   "GPIOA" "_" "IDR"   =   "GPIOA_IDR"
 */

/* name_str is a string literal; periph is the expanded pointer expression.
 * Keeping them separate is what lets # fire at the right level. */
#define LUA_REG(L, name_str, periph, field)                                 \
    do {                                                                     \
        lua_pushinteger((L), (lua_Integer)(uintptr_t)&((periph)->field));   \
        lua_setglobal((L), name_str "_" #field);                            \
    } while (0)

#define LUA_AFR(L, name_str, periph)                                        \
    do {                                                                     \
        lua_pushinteger((L), (lua_Integer)(uintptr_t)&((periph)->AFR[0]));  \
        lua_setglobal((L), name_str "_AFRL");                               \
        lua_pushinteger((L), (lua_Integer)(uintptr_t)&((periph)->AFR[1]));  \
        lua_setglobal((L), name_str "_AFRH");                               \
    } while (0)

/* #P fires here, where P is still the bare token GPIOA/GPIOB/etc.
 * The expanded pointer form reaches LUA_REG separately for &(periph)->field. */
#define GPIO_PORT_REGS(L, P)        \
    LUA_REG(L, #P, P, MODER);      \
    LUA_REG(L, #P, P, OTYPER);     \
    LUA_REG(L, #P, P, OSPEEDR);    \
    LUA_REG(L, #P, P, PUPDR);      \
    LUA_REG(L, #P, P, IDR);        \
    LUA_REG(L, #P, P, ODR);        \
    LUA_REG(L, #P, P, BSRR);       \
    LUA_REG(L, #P, P, LCKR);       \
    LUA_AFR(L, #P, P)

static void luahal_register_regs(lua_State *L)
{
    /* GPIO — every port present on STM32F411 */
    GPIO_PORT_REGS(L, GPIOA);
    GPIO_PORT_REGS(L, GPIOB);
    GPIO_PORT_REGS(L, GPIOC);
    GPIO_PORT_REGS(L, GPIOD);
    GPIO_PORT_REGS(L, GPIOE);
    GPIO_PORT_REGS(L, GPIOH);

    /* RCC — the registers you always need for peripheral bring-up */
    LUA_REG(L, "RCC", RCC, CR);
    LUA_REG(L, "RCC", RCC, CFGR);
    LUA_REG(L, "RCC", RCC, AHB1ENR);
    LUA_REG(L, "RCC", RCC, AHB2ENR);
    LUA_REG(L, "RCC", RCC, APB1ENR);
    LUA_REG(L, "RCC", RCC, APB2ENR);
    LUA_REG(L, "RCC", RCC, AHB1RSTR);
    LUA_REG(L, "RCC", RCC, APB1RSTR);
    LUA_REG(L, "RCC", RCC, APB2RSTR);
}

#undef GPIO_PORT_REGS
/* LUA_REG and LUA_AFR are intentionally left defined — they're useful
 * if you want to add more peripheral constants later in this file. */

/* ============================================================
 *  Registration
 * ============================================================ */

static const luaL_Reg hal_funcs[] = {
    { "rd",      l_rd      },
    { "wr",      l_wr      },
    { "setbits", l_setbits },
    { "clrbits", l_clrbits },
    { "pin",     l_pin     },
    { "dw",      l_dw      },
    { "dr",      l_dr      },
    { "ticks",   l_ticks   },
    { "sleep",   l_sleep   },
    { "hex",     l_hex     },
    { "help",    l_help    },
    { "cmds",    l_cmds    },
    { "ver",     l_ver     },
    { "reset",   l_reset   },
    { NULL,      NULL      },
};

void luahal_register(lua_State *L)
{
    /* Bind every HAL function as a global so the REPL can call it bare. */
    for (const luaL_Reg *f = hal_funcs; f->name != NULL; f++)
        lua_register(L, f->name, f->func);

    /* Populate register-address constants (GPIOA_IDR, RCC_AHB1ENR, …). */
    luahal_register_regs(L);
}