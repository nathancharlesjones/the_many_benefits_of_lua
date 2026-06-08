/* lua_hal_RP2040.c
 *
 * RP2040 / Raspberry Pi Pico HAL back-end for the Lua binding layer.
 *
 * Drop-in replacement for lua_hal.c (the stub).  Compile exactly one of the
 * two into your project; both export the same luahal_register() symbol.
 *
 * Extra globals registered beyond the base command set:
 *   ticks()     — milliseconds since boot
 *   usecs()     — microseconds since boot (time_us_32); wraps at ~71 min
 *   sleep(ms)   — busy-wait ms milliseconds; polls stdin for Ctrl-C (0x03)
 *                 so loops can be interrupted from the terminal
 *   hex(n)      — format an integer as "0x%08X"; handy for register values
 *
 * GPIO API differences from the STM32F4 version:
 *   The RP2040 has a single flat GPIO bank (GPIO0–GPIO29); there is no port
 *   letter.  Functions that took (port, pin) on STM32 take (pin) here.
 *
 *   pin(n, mode, [pull], [func])
 *     n     0-29
 *     mode  "in" | "out" | "func" | "analog"
 *     pull  "none" | "up" | "down" | "both"   (default "none")
 *     func  name string, required when mode == "func":
 *           "sio" | "uart" | "spi" | "i2c" | "pwm"
 *           | "pio0" | "pio1" | "usb" | "null"
 *
 *   dw(n, val)    digital write pin n (0 or 1)
 *   dr(n)         digital read  pin n -> 0 or 1
 *
 * Register-address globals (see luahal_register_regs):
 *   SIO_GPIO_IN, SIO_GPIO_OUT, SIO_GPIO_SET, SIO_GPIO_CLR, SIO_GPIO_TOGL
 *   SIO_GPIO_OE,  SIO_GPIO_OE_SET, SIO_GPIO_OE_CLR, SIO_GPIO_OE_TOGL
 *   IO_BANK0_GPIO{n}_CTRL   (n = 0-29)  — function-select / input-override
 *   PADS_BANK0_GPIO{n}      (n = 0-29)  — drive/pull/input-enable per pin
 *   RESETS_RESET, RESETS_WDSEL, RESETS_RESET_DONE
 *   UART0_DR,  UART0_RSR,  UART0_FR,   UART0_IBRD, UART0_FBRD,
 *   UART0_LCR_H, UART0_CR, UART0_IFLS, UART0_IMSC, UART0_ICR
 *   (same set for UART1_*)
 *   TIMER_TIMEHR, TIMER_TIMELR, TIMER_TIMERAWH, TIMER_TIMERAWL,
 *   TIMER_ALARM0..3, TIMER_ARMED, TIMER_INTR
 *
 * These are plain Lua integers (byte addresses), so:
 *   hex(rd(SIO_GPIO_IN))              -- read all GPIO inputs at once
 *   rd(SIO_GPIO_IN) & (1<<25)         -- test GPIO25 (onboard LED on Pico)
 *   rd(IO_BANK0_GPIO25_CTRL)          -- read GPIO25 function select
 *   wr(SIO_GPIO_SET, 1<<25)           -- drive GPIO25 high atomically
 *   hex(rd(PADS_BANK0_GPIO25))        -- read pad config for GPIO25
 * all work directly from the REPL without any special parsing in C.
 *
 * CMakeLists.txt additions needed beyond pico_stdlib:
 *   target_link_libraries(... hardware_gpio hardware_timer
 *                             hardware_watchdog pico_bootrom)
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/gpio.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "hardware/structs/sio.h"
#include "hardware/structs/iobank0.h"
#include "hardware/structs/pads_bank0.h"
#include "hardware/structs/resets.h"
#include "hardware/structs/uart.h"
#include "hardware/structs/timer.h"

#include "lua.h"
#include "lauxlib.h"
#include "lua_hal.h"

/* ============================================================
 *  Internal helpers
 * ============================================================ */

/** Validate a GPIO pin number (0-29).  Raises a Lua error if out of range. */
static void check_pin(lua_State *L, lua_Integer n, int argidx)
{
    luaL_argcheck(L, n >= 0 && n <= 29, argidx, "GPIO pin must be 0-29");
}

/** Map a func-mode string to a gpio_function_t.
 *  Returns GPIO_FUNC_NULL and sets *ok = 0 if the string is unrecognised. */
static gpio_function_t func_string_to_enum(const char *s, int *ok)
{
    static const struct { const char *name; gpio_function_t fn; } map[] = {
        { "sio",  GPIO_FUNC_SIO  },
        { "uart", GPIO_FUNC_UART },
        { "spi",  GPIO_FUNC_SPI  },
        { "i2c",  GPIO_FUNC_I2C  },
        { "pwm",  GPIO_FUNC_PWM  },
        { "pio0", GPIO_FUNC_PIO0 },
        { "pio1", GPIO_FUNC_PIO1 },
        { "usb",  GPIO_FUNC_USB  },
        { "null", GPIO_FUNC_NULL },
        { NULL,   0              },
    };
    *ok = 1;
    for (int i = 0; map[i].name != NULL; i++) {
        if (!strcmp(s, map[i].name))
            return map[i].fn;
    }
    *ok = 0;
    return GPIO_FUNC_NULL;
}

/* ============================================================
 *  Raw register access
 *  (identical logic to STM32F4 version — hardware-agnostic)
 * ============================================================ */

/*
 * rd(addr, [n=1], [width=32])
 *
 * Read n consecutive memory-mapped locations of the given bit-width starting
 * at addr.  Returns n integers on the Lua stack.
 *
 *   val        = rd(SIO_GPIO_IN)            -- read all GPIO inputs
 *   val        = rd(0xD0000000)             -- SIO base by hand
 *   b0, b1, b2 = rd(0x20000000, 3, 8)      -- three bytes from SRAM
 *   hex(rd(UART0_FR))                       -- pretty-print flag register
 */
static int l_rd(lua_State *L)
{
    lua_Integer addr  = luaL_checkinteger(L, 1);
    lua_Integer n     = luaL_optinteger (L, 2, 1);
    lua_Integer width = luaL_optinteger (L, 3, 32);

    luaL_argcheck(L, width == 8 || width == 16 || width == 32, 3,
                  "width must be 8, 16, or 32");
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
 *   wr(SIO_GPIO_SET, 1<<25)      -- set GPIO25 high atomically
 *   wr(UART0_DR, 0x41)           -- poke 'A' into the UART data register
 *
 * Note: the SIO peripheral provides dedicated atomic set/clr/xor aliases
 * (SIO_GPIO_SET, SIO_GPIO_CLR, SIO_GPIO_TOGL) so wr() to those is already
 * atomic.  For other peripherals use setbits/clrbits for RMW operations.
 */
static int l_wr(lua_State *L)
{
    lua_Integer addr  = luaL_checkinteger(L, 1);
    lua_Integer val   = luaL_checkinteger(L, 2);
    lua_Integer width = luaL_optinteger (L, 3, 32);

    luaL_argcheck(L, width == 8 || width == 16 || width == 32, 3,
                  "width must be 8, 16, or 32");

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
 *   setbits(RESETS_RESET, 1<<22)    -- assert reset for UART1
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
 *   clrbits(RESETS_RESET, 1<<22)    -- deassert reset for UART1
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
 * pin(n, mode, [pull="none"], [func])
 *
 * Configure one GPIO pin.  Handles the four main use-cases cleanly:
 *
 *   pin(25, "out")                   -- GPIO25 push-pull output (onboard LED)
 *   pin(15, "in", "up")              -- GPIO15 input, pull-up enabled
 *   pin(0,  "func", "none", "uart")  -- GPIO0 routed to UART0 TX
 *   pin(26, "analog")                -- GPIO26 isolated for ADC0 use
 *
 * mode:
 *   "in"     — digital input via SIO; gpio_init + GPIO_IN
 *   "out"    — digital output via SIO; gpio_init + GPIO_OUT
 *   "func"   — peripheral function mux; requires func argument
 *   "analog" — disable digital input buffer + pulls (ready for ADC)
 *
 * pull: "none" | "up" | "down" | "both"
 *   "both" enables the weak keeper (both pull-up and pull-down simultaneously,
 *   which on RP2040 keeps the line at whatever level it was last driven to).
 *
 * func (required for mode "func"):
 *   "sio" | "uart" | "spi" | "i2c" | "pwm" | "pio0" | "pio1" | "usb" | "null"
 *
 * Consult the RP2040 datasheet Table 2 for the mapping of func names to
 * specific pin functions (e.g. GPIO0 "uart" = UART0 TX).
 */
static int l_pin(lua_State *L)
{
    static const char *mode_opts[] = { "in", "out", "func", "analog", NULL };
    static const char *pull_opts[] = { "none", "up", "down", "both", NULL };

    lua_Integer n    = luaL_checkinteger(L, 1);
    int         mode = luaL_checkoption (L, 2, NULL,   mode_opts);
    int         pull = luaL_checkoption (L, 3, "none", pull_opts);

    check_pin(L, n, 1);

    uint pin = (uint)n;

    /* Apply pull configuration first — valid for all modes */
    switch (pull) {
        case 0: gpio_set_pulls(pin, false, false); break;   /* none */
        case 1: gpio_set_pulls(pin, true,  false); break;   /* up   */
        case 2: gpio_set_pulls(pin, false, true ); break;   /* down */
        case 3: gpio_set_pulls(pin, true,  true ); break;   /* both */
    }

    switch (mode) {
        case 0:  /* "in" */
            gpio_init(pin);
            gpio_set_dir(pin, GPIO_IN);
            break;

        case 1:  /* "out" */
            gpio_init(pin);
            gpio_set_dir(pin, GPIO_OUT);
            break;

        case 2: {  /* "func" */
            const char *func_str = luaL_checkstring(L, 4);
            int ok;
            gpio_function_t fn = func_string_to_enum(func_str, &ok);
            if (!ok)
                return luaL_argerror(L, 4,
                    "unknown func; use: sio uart spi i2c pwm pio0 pio1 usb null");
            gpio_set_function(pin, fn);
            break;
        }

        case 3:  /* "analog" */
            /* Disable the input buffer and all pulls to isolate the pad.
             * This is what adc_gpio_init() does internally; reproduced here
             * so callers don't need to link hardware_adc just for setup. */
            gpio_set_function(pin, GPIO_FUNC_NULL);
            gpio_set_input_enabled(pin, false);
            gpio_set_pulls(pin, false, false);   /* override pull setting */
            break;
    }

    return 0;
}

/*
 * dw(n, val)
 *
 * Drive GPIO pin n high (val=1) or low (val=0).
 *
 *   dw(25, 1)    -- GPIO25 high -> onboard LED on
 *   dw(25, 0)    -- GPIO25 low  -> onboard LED off
 */
static int l_dw(lua_State *L)
{
    lua_Integer n   = luaL_checkinteger(L, 1);
    lua_Integer val = luaL_checkinteger(L, 2);

    check_pin(L, n, 1);
    luaL_argcheck(L, val == 0 || val == 1, 2, "val must be 0 or 1");

    gpio_put((uint)n, (bool)val);
    return 0;
}

/*
 * dr(n)  ->  0 or 1
 *
 * Sample the current input level of GPIO pin n.
 *
 *   dr(15)   -- read GPIO15 (e.g. a button)
 */
static int l_dr(lua_State *L)
{
    lua_Integer n = luaL_checkinteger(L, 1);
    check_pin(L, n, 1);
    lua_pushinteger(L, gpio_get((uint)n) ? 1 : 0);
    return 1;
}

/* ============================================================
 *  Timing utilities
 * ============================================================ */

/*
 * ticks()  ->  integer
 *
 * Milliseconds elapsed since boot.  Wraps at ~49 days.
 * Matches the STM32 HAL_GetTick() convention for portable Lua scripts.
 *
 *   local t0 = ticks()
 *   while ticks()-t0 < 5000 do dw(25,1) sleep(250) dw(25,0) sleep(250) end
 */
static int l_ticks(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)to_ms_since_boot(get_absolute_time()));
    return 1;
}

/*
 * usecs()  ->  integer
 *
 * Microseconds elapsed since boot (lower 32 bits only — wraps at ~71 min).
 * Use for short-interval timing where millisecond resolution isn't enough.
 *
 *   local t = usecs() ; wr(SIO_GPIO_SET, 1<<15) ; print(usecs()-t, "us")
 */
static int l_usecs(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)time_us_32());
    return 1;
}

/*
 * sleep(ms)
 *
 * Block for ms milliseconds, polling stdin every millisecond for Ctrl-C
 * (0x03).  Receiving Ctrl-C raises a Lua "interrupted" error which the REPL
 * prints and recovers from normally.
 *
 * Rule: any Lua loop that should be keyboard-breakable must call sleep() at
 * least once per iteration — even sleep(1) suffices.
 *
 * Note: bytes arriving during the sleep (other than Ctrl-C) are consumed and
 * discarded.  This prevents the USB CDC receive buffer from filling up behind
 * a running loop, at the cost of losing keystrokes typed mid-loop.
 */
static int l_sleep(lua_State *L)
{
    lua_Integer ms = luaL_checkinteger(L, 1);
    if (ms < 0) ms = 0;

    uint32_t start = to_ms_since_boot(get_absolute_time());
    while (to_ms_since_boot(get_absolute_time()) - start < (uint32_t)ms) {
        int c = getchar_timeout_us(0);
        if (c == 0x03)
            return luaL_error(L, "interrupted");
        sleep_ms(1);
    }
    return 0;
}

/* ============================================================
 *  Formatting utility
 * ============================================================ */

/*
 * hex(n)  ->  string "0xXXXXXXXX"
 *
 * The REPL prints integers in decimal by default, which is unpleasant for
 * register values.  Wrap any expression in hex() at the prompt:
 *
 *   hex(rd(SIO_GPIO_IN))              -->  "0x02000000"
 *   hex(rd(IO_BANK0_GPIO25_CTRL))     -->  "0x00000005"
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
        printf("help: no detailed entry for '%s' — see lua_hal_RP2040.c\r\n", cmd);
        return 0;
    }
    printf(
        "HAL commands (RP2040 / Raspberry Pi Pico)\r\n"
        "  rd(addr,[n],[w])                 read n regs, width w=8/16/32\r\n"
        "  wr(addr,val,[w])                 write register\r\n"
        "  setbits(addr,mask)               RMW: OR in mask\r\n"
        "  clrbits(addr,mask)               RMW: AND out mask\r\n"
        "  pin(n,mode,[pull],[func])        configure GPIO pin\r\n"
        "    mode: in|out|func|analog\r\n"
        "    pull: none|up|down|both\r\n"
        "    func: sio|uart|spi|i2c|pwm|pio0|pio1|usb|null\r\n"
        "  dw(n,val)                        digital write (0 or 1)\r\n"
        "  dr(n)                            digital read  -> 0 or 1\r\n"
        "  ticks()                          ms since boot\r\n"
        "  usecs()                          us since boot (32-bit, ~71min)\r\n"
        "  sleep(ms)                        delay; Ctrl-C interrupts loop\r\n"
        "  hex(n)                           format integer as 0xXXXXXXXX\r\n"
        "  help([cmd])                      this message\r\n"
        "  cmds()                           machine-parseable list\r\n"
        "  ver()                            firmware version\r\n"
        "  reset([mode])                    reset; mode=boot|dfu\r\n"
        "Register constants: SIO_GPIO_IN/OUT/SET/CLR/TOGL/OE/OE_SET/OE_CLR\r\n"
        "  IO_BANK0_GPIO{n}_CTRL (n=0-29)   PADS_BANK0_GPIO{n} (n=0-29)\r\n"
        "  RESETS_RESET/WDSEL/RESET_DONE\r\n"
        "  UART0_*/UART1_*  TIMER_TIMEHR/TIMELR/ALARMx/...\r\n"
    );
    return 0;
}

static int l_cmds(lua_State *L)
{
    (void)L;
    printf("rd,wr,setbits,clrbits,pin,dw,dr,ticks,usecs,sleep,hex,"
           "help,cmds,ver,reset\r\n");
    return 0;
}

static int l_ver(lua_State *L)
{
    (void)L;
    printf("fw=0.1.0 proto=1 target=RP2040 build=" __DATE__ " " __TIME__ "\r\n");
    return 0;
}

/* ============================================================
 *  Control
 * ============================================================ */

/*
 * reset([mode])
 *
 *   reset()         — software reset via NVIC_SystemReset (plain reboot)
 *   reset("boot")   — reboot into USB BOOTSEL mass-storage mode
 *   reset("dfu")    — alias for "boot" (same target; no separate DFU ROM)
 *
 * After reset("boot") the Pico appears as the RPI-RP2 USB drive; drag-drop
 * a new .uf2 to reflash, or press BOOTSEL again to return to normal boot.
 */
static int l_reset(lua_State *L)
{
    static const char *reset_opts[] = { "reset", "boot", "dfu", NULL };
    int mode = luaL_checkoption(L, 1, "reset", reset_opts);

    /* Give any pending printf output a chance to drain before going dark */
    stdio_flush();
    sleep_ms(10);

    if (mode > 0) {
        /* reset_usb_boot(gpio_activity_mask, disable_interface_mask)
         * Both zero = default Pico BOOTSEL behaviour: LED on GPIO25 blinks,
         * both USB and PICOBOOT interfaces enabled. */
        reset_usb_boot(0, 0);
    } else {
        watchdog_reboot(0, 0, 0);
    }
    return 0;   /* unreachable */
}

/* ============================================================
 *  Register-address constants -> Lua globals
 * ============================================================
 *
 * LUA_REG(L, name_str, periph_ptr, field) pushes the byte address of
 * periph_ptr->field as a Lua integer global named "name_str_field".
 *
 * Because the address is derived from &periph->field (the live C struct),
 * it is always correct for the actual peripheral map — no hand-coding.
 *
 *   LUA_REG(L, "SIO", sio_hw, gpio_in)   =>   SIO_GPIO_IN = 0xD0000004
 *
 * The name_str token must be a string literal; the field token is
 * stringified by # inside the macro.  Upper-casing of the field name is
 * done by a second helper macro so callers stay readable.
 */

#define LUA_REG(L, name_str, periph_ptr, field)                              \
    do {                                                                      \
        lua_pushinteger((L),                                                  \
            (lua_Integer)(uintptr_t)&((periph_ptr)->field));                  \
        lua_setglobal((L), name_str "_" #field);                              \
    } while (0)

/* Variant that lets the caller supply an explicit global name string,
 * used for fields whose C names differ from the desired Lua constant name
 * (e.g. alarm[] array elements). */
#define LUA_REG_NAMED(L, global_name, addr_expr)                             \
    do {                                                                      \
        lua_pushinteger((L), (lua_Integer)(uintptr_t)(addr_expr));            \
        lua_setglobal((L), global_name);                                      \
    } while (0)

/* Convenience: upper-case the struct field name via stringification.
 * C preprocessor stringification preserves the case of the token, so
 * field names that are already upper-case (e.g. IBRD) stay upper-case,
 * and lower-case ones (e.g. gpio_in) become the literal "gpio_in" in the
 * global name.  We accept this; the full name (e.g. "SIO_gpio_in") is
 * still unambiguous and consistent. */

static void luahal_register_regs(lua_State *L)
{
    /* ------------------------------------------------------------------
     * SIO — Single-cycle IO: fast GPIO path, always enabled
     *
     * These are the registers you'd use for bulk GPIO manipulation.
     * Individual pin control goes through IO_BANK0 and PADS_BANK0 below.
     *
     * gpio_in     read-only snapshot of all GPIO input levels
     * gpio_out    read/write output latch
     * gpio_set    write-1-to-set output bits atomically (no RMW needed)
     * gpio_clr    write-1-to-clear output bits atomically
     * gpio_togl   write-1-to-toggle output bits atomically
     * gpio_oe     output-enable register (1 = output, 0 = input)
     * gpio_oe_set atomic set of OE bits
     * gpio_oe_clr atomic clear of OE bits
     * gpio_oe_togl atomic toggle of OE bits
     * ------------------------------------------------------------------ */
    LUA_REG(L, "SIO", sio_hw, gpio_in);
    LUA_REG(L, "SIO", sio_hw, gpio_out);
    LUA_REG(L, "SIO", sio_hw, gpio_set);
    LUA_REG(L, "SIO", sio_hw, gpio_clr);
    LUA_REG(L, "SIO", sio_hw, gpio_togl);
    LUA_REG(L, "SIO", sio_hw, gpio_oe);
    LUA_REG(L, "SIO", sio_hw, gpio_oe_set);
    LUA_REG(L, "SIO", sio_hw, gpio_oe_clr);
    LUA_REG(L, "SIO", sio_hw, gpio_oe_togl);

    /* ------------------------------------------------------------------
     * IO_BANK0 — per-pin function select and input-override registers
     *
     * Layout: STATUS at base + n*8,  CTRL at base + n*8 + 4
     * The CTRL register holds the FUNCSEL field (bits 4:0) and input-override
     * fields.  See RP2040 datasheet Table 276.
     *
     * We expose only the CTRL register for each pin (the STATUS register is
     * read-only and mainly useful for debug; read it via rd() with the address
     * IO_BANK0_GPIO{n}_CTRL - 4 if needed).
     *
     * Example:
     *   hex(rd(IO_BANK0_GPIO0_CTRL))     -- FUNCSEL 2 = UART0 TX
     *   rd(IO_BANK0_GPIO0_CTRL) & 0x1F   -- extract FUNCSEL bits
     * ------------------------------------------------------------------ */
    {
        char name[32];
        for (int i = 0; i <= 29; i++) {
            snprintf(name, sizeof(name), "IO_BANK0_GPIO%d_CTRL", i);
            LUA_REG_NAMED(L, name, &iobank0_hw->io[i].ctrl);
        }
    }

    /* ------------------------------------------------------------------
     * PADS_BANK0 — per-pin pad control
     *
     * Each register controls drive strength, slew rate, input enable,
     * schmitt trigger, and pull-up/down for one pin.
     * See RP2040 datasheet Table 281.
     *
     * Example:
     *   hex(rd(PADS_BANK0_GPIO25))            -- read pad config for GPIO25
     *   setbits(PADS_BANK0_GPIO25, 1<<6)      -- set OD (output disable)
     *   clrbits(PADS_BANK0_GPIO25, 1<<6)      -- clear OD
     *
     * Bit field reference (per pin):
     *   [7]   OD    — output disable
     *   [6]   IE    — input enable
     *   [5:4] DRIVE — drive strength: 00=2mA 01=4mA 10=8mA 11=12mA
     *   [3]   PUE   — pull-up enable
     *   [2]   PDE   — pull-down enable
     *   [1]   SCHMITT — schmitt trigger enable
     *   [0]   SLEWFAST — slew rate
     * ------------------------------------------------------------------ */
    {
        char name[32];
        for (int i = 0; i <= 29; i++) {
            snprintf(name, sizeof(name), "PADS_BANK0_GPIO%d", i);
            LUA_REG_NAMED(L, name, &pads_bank0_hw->io[i]);
        }
    }

    /* ------------------------------------------------------------------
     * RESETS — peripheral reset control
     *
     * Each bit in RESETS_RESET corresponds to one peripheral subsystem.
     * Assert (set to 1) to hold in reset; deassert (clear to 0) to release.
     * RESETS_RESET_DONE reflects which peripherals have completed their
     * reset-release sequence.
     *
     * Example — manually reset and release UART1 (bit 22):
     *   setbits(RESETS_RESET, 1<<22)           -- assert UART1 reset
     *   clrbits(RESETS_RESET, 1<<22)           -- release UART1 reset
     *   while rd(RESETS_RESET_DONE)&(1<<22)==0 do end  -- wait for done
     * ------------------------------------------------------------------ */
    LUA_REG(L, "RESETS", resets_hw, reset);
    LUA_REG(L, "RESETS", resets_hw, wdsel);
    LUA_REG(L, "RESETS", resets_hw, reset_done);

    /* ------------------------------------------------------------------
     * UART0 / UART1
     *
     * Standard ARM PrimeCell PL011 UART register set.
     * UART0 base: 0x40034000   UART1 base: 0x40038000
     *
     * Key registers:
     *   DR     — data register (read to receive, write to transmit)
     *   RSR    — receive status / error clear
     *   FR     — flag register: TXFE[7] RXFF[6] TXFF[5] RXFE[4] BUSY[3]
     *   IBRD   — integer baud rate divisor
     *   FBRD   — fractional baud rate divisor
     *   LCR_H  — line control: word length, parity, stop bits, FIFOs
     *   CR     — control: UARTEN[0], TXE[8], RXE[9]
     *   IFLS   — FIFO level select for interrupts
     *   IMSC   — interrupt mask (1 = masked/disabled)
     *   ICR    — interrupt clear (write 1 to clear)
     * ------------------------------------------------------------------ */
    LUA_REG(L, "UART0", uart0_hw, dr);
    LUA_REG(L, "UART0", uart0_hw, rsr);
    LUA_REG(L, "UART0", uart0_hw, fr);
    LUA_REG(L, "UART0", uart0_hw, ibrd);
    LUA_REG(L, "UART0", uart0_hw, fbrd);
    LUA_REG(L, "UART0", uart0_hw, lcr_h);
    LUA_REG(L, "UART0", uart0_hw, cr);
    LUA_REG(L, "UART0", uart0_hw, ifls);
    LUA_REG(L, "UART0", uart0_hw, imsc);
    LUA_REG(L, "UART0", uart0_hw, icr);

    LUA_REG(L, "UART1", uart1_hw, dr);
    LUA_REG(L, "UART1", uart1_hw, rsr);
    LUA_REG(L, "UART1", uart1_hw, fr);
    LUA_REG(L, "UART1", uart1_hw, ibrd);
    LUA_REG(L, "UART1", uart1_hw, fbrd);
    LUA_REG(L, "UART1", uart1_hw, lcr_h);
    LUA_REG(L, "UART1", uart1_hw, cr);
    LUA_REG(L, "UART1", uart1_hw, ifls);
    LUA_REG(L, "UART1", uart1_hw, imsc);
    LUA_REG(L, "UART1", uart1_hw, icr);

    /* ------------------------------------------------------------------
     * TIMER — 64-bit microsecond counter + 4 alarm comparators
     *
     * Reading TIMEHR latches TIMELR; always read TIMEHR first, then TIMELR.
     * TIMERAWH/TIMERAWL read without latching — safe for polling lower 32.
     *
     * Example:
     *   local h = rd(TIMER_TIMEHR)          -- latch upper, then:
     *   local l = rd(TIMER_TIMELR)          -- read lower (latched value)
     *   print(h, l)                         -- microseconds since boot
     *
     *   wr(TIMER_ALARM0, rd(TIMER_TIMERAWL) + 1000000) -- alarm in 1 s
     * ------------------------------------------------------------------ */
    LUA_REG(L, "TIMER", timer_hw, timehr);
    LUA_REG(L, "TIMER", timer_hw, timelr);
    LUA_REG(L, "TIMER", timer_hw, timerawh);
    LUA_REG(L, "TIMER", timer_hw, timerawl);
    LUA_REG(L, "TIMER", timer_hw, armed);
    LUA_REG(L, "TIMER", timer_hw, intr);
    LUA_REG(L, "TIMER", timer_hw, inte);

    LUA_REG_NAMED(L, "TIMER_ALARM0", &timer_hw->alarm[0]);
    LUA_REG_NAMED(L, "TIMER_ALARM1", &timer_hw->alarm[1]);
    LUA_REG_NAMED(L, "TIMER_ALARM2", &timer_hw->alarm[2]);
    LUA_REG_NAMED(L, "TIMER_ALARM3", &timer_hw->alarm[3]);
}

#undef LUA_REG
#undef LUA_REG_NAMED

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
    { "usecs",   l_usecs   },
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

    /* Populate register-address constants. */
    luahal_register_regs(L);
}
