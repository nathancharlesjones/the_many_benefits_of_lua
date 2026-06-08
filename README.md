# The many benefits of Lua for embedded systems
Companion repo for the video ["The many benefits of Lua for embedded systems"]() for DigiKey

The "many benefits of Lua" for embedded systems are, in short, that Lua has a similar syntax and features to MicroPython (such as having a live REPL you can use to test code on the device) while being **50-65% smaller**, **5x faster**, and **significantly easier to port** to a new microcontroller ("hours to days" compared to "days to weeks").

All instructions below are for Linux.

## Getting Lua on your computer

Note: This is not strictly necessary to be able to run Lua on a microcontroller. It was, however, the first demonstration in the video so I'm including it here for completeness.

1. Install via package manager (which is one subversion behind the latest version; 5.4 instead of 5.5).

```bash
sudo apt install lua5.4
```

2. Test it out

```bash
$ lua5.4
Lua 5.4.8  Copyright (C) 1994-2025 Lua.org, PUC-Rio
> print(2+2)
4
> t = {3,1,4,1,5,9}; table.sort(t); print(table.concat(t, ", "))
1, 1, 3, 4, 5, 9
> 
```

## Running a basic Lua REPL on an STM32

Sample project [`lua_on_STM32`](lua_on_STM32).

1. Download the Lua source code and clone the [EmbeddedCLI](https://github.com/AndreRenaud/EmbeddedCLI) library.

```bash
cd #install location
curl -L -R -O https://www.lua.org/ftp/lua-5.5.0.tar.gz
tar zxf lua-5.5.0.tar.gz
rm -rf lua-5.5.0.tar.gz
```

```bash
cd #desired folder location
git clone https://github.com/AndreRenaud/EmbeddedCLI
```

2. Make a CubeMX project

    - Set heap to 0x10000 (64 kB) to start

3. Make the following changes to the resulting Makefile ("Lua" is the location of `lua-5.5.0/src` and "ECLI" is the path to the Embedded CLI folder)

```make
# Add to C_SOURCES
C_SOURCES = \
Lua/lapi.c \
Lua/lauxlib.c \
Lua/lbaselib.c \
Lua/lcode.c \
Lua/lcorolib.c \
Lua/lctype.c \
Lua/ldblib.c \
Lua/ldebug.c \
Lua/ldo.c \
Lua/ldump.c \
Lua/lfunc.c \
Lua/lgc.c \
Lua/llex.c \
Lua/lmathlib.c \
Lua/lmem.c \
Lua/lobject.c \
Lua/lopcodes.c \
Lua/lparser.c \
Lua/lstate.c \
Lua/lstring.c \
Lua/lstrlib.c \
Lua/ltable.c \
Lua/ltablib.c \
Lua/ltm.c \
Lua/lundump.c \
Lua/lvm.c \
Lua/lzio.c \
ECLI/embedded_cli.c

# Add to C_DEFS
C_DEFS =  \
-DLUAI_MAXSTACK=500 \
-DMINSTRTABSIZE=32

# Add to C_INCLUDES
C_INCLUDES = \
-ILua \
-IECLI

# Modify compilation to use local luaconf.h
# Instead of
# CFLAGS += $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections
# use
CFLAGS += $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections \
          -include Core/Inc/luaconf.h
          
# Add to LDFLAGS
LDFLAGS += -u _printf_float
          
# Ensure the assembler uses the actual ASFLAGS (not CFLAGS)
$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(ASFLAGS) $< -o $@
$(BUILD_DIR)/%.o: %.S Makefile | $(BUILD_DIR)
	$(AS) -c $(ASFLAGS) $< -o $@
```

4. Copy `luaconf.h` to `Core/Inc` and make the following changes.

```c
// Uncomment line 141
#define LUA_32BITS

// Add under "Local configuration" at the end of the file
#undef  LUA_PATH_DEFAULT
#define LUA_PATH_DEFAULT  ""

#undef  LUA_CPATH_DEFAULT
#define LUA_CPATH_DEFAULT ""

#define LUA_IDSIZE 32           // Limits source location desc in debug info (Default: 60)
#define LUAL_BUFFERSIZE 64      // Max string length for str ops (Default: 256)
#define LUAI_MAXALIGN lua_Number n; void* s; lua_Integer i; long l
                                // Above: removed double (Numbers are floats)
```

5. Add to `main.c`:

```c
/* USER CODE BEGIN Includes */
#include <stdio.h>
#include <string.h>
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "embedded_cli.h"
/* USER CODE END Includes */

/* USER CODE BEGIN PV */
static lua_State *L;
/* USER CODE END PV */

/* USER CODE BEGIN 0 */
int __io_getchar(void)
{
  uint8_t ch;
  (void) HAL_UART_Receive(&huart2, &ch, 1, HAL_MAX_DELAY);
  return (int)ch;
}

int __io_putchar(int ch)
{
  (void) HAL_UART_Transmit(&huart2, (uint8_t *) &ch, 1, HAL_MAX_DELAY);
  return ch;
}

static void put_char(void *data, char ch, bool is_last)
{
    (void)data; (void)is_last;
    __io_putchar(ch);
}
/* USER CODE END 0 */

// Inside main()
/* USER CODE BEGIN WHILE */
L = luaL_newstate();
luaL_requiref(L, "_G",     luaopen_base,   1); lua_pop(L, 1);
luaL_requiref(L, "math",   luaopen_math,   1); lua_pop(L, 1);
luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
luaL_requiref(L, "table",  luaopen_table,  1); lua_pop(L, 1);
// Optional: Include coroutines and debug support
// luaL_requiref(L, "coroutine",  luaopen_coroutine,  1); lua_pop(L, 1);
// luaL_requiref(L, "debug",      luaopen_debug,  1);     lua_pop(L, 1);

printf("Lua REPL ready\r\n");
fflush(stdout); 
  
struct embedded_cli cli;
embedded_cli_init(&cli, "> ", put_char, NULL);
embedded_cli_prompt(&cli);
  
while (1)
{
  if (embedded_cli_insert_char(&cli, __io_getchar()))
  {
      const char *line = embedded_cli_get_line(&cli);

      if (LUA_OK != luaL_dostring(L, line))
      {
          const char *msg = lua_tostring(L, -1);
          printf("error: %s\n", msg ? msg : "(non-string error)");
          lua_pop(L, 1);
      }
        
      lua_settop(L, 0);
      embedded_cli_prompt(&cli);
  }
  /* USER CODE END WHILE */
```

6. Run `make`, flash however you're most comfortable, and then connect to the serial port to begin using Lua on your device.

## Running a Lua REPL written in Lua on an STM32 (with HAL)

Sample project [`lua_in_lua_on_STM32`](lua_in_lua_on_STM32).

1. Create a new project (following steps 1-4 above) or modify the one you just made.

2. Create a simple HAL you can call from Lua and add the source file to your makefile and the header to `main.c`.

	- A complete example that Claude helped generate for me is [lua_hal_STM32F4.c](lua_in_lua_on_STM32/Core/Src/lua_hal_STM32F4.c). This implementation includes the functions shown in the video (rd, wr, setbits, clrbits, pin, dr, dw, ticks, and sleep) and also converts string names for device registers (e.g. the "GPIOA_MODER" in `rd(GPIOA_MODER)`) into their register addresses.

	- This file doesn't need to be super complicated, though. A reduced version of this is [lua_hal_STM32F4_reduced.c](lua_in_lua_on_STM32/Core/Src/lua_hal_STM32F4_reduced.c). This implementation only shows a few of the functions (rd, wr, setbits, clrbits, dr, and dw) and excludes some useful features like error checking argument values and converting string names to device register addresses.

3. Make the following changes to the `main.c` described above:

```c
// Add to /* USER CODE BEGIN Includes */
#include "lua_hal.h" 

// Add to /* USER CODE BEGIN PV */
static struct embedded_cli cli;

// Add to /* USER CODE BEGIN 0 */
int l_cli_prompt(lua_State* L)
{
  embedded_cli_prompt(&cli);
  return 0;
}

int l_get_line(lua_State* L)
{
  if (embedded_cli_insert_char(&cli, __io_getchar()))
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

// Replace /* USER CODE BEGIN WHILE */ with
L = luaL_newstate();
luaL_requiref(L, "_G",     luaopen_base,   1); lua_pop(L, 1);
luaL_requiref(L, "math",   luaopen_math,   1); lua_pop(L, 1);
luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
luaL_requiref(L, "table",  luaopen_table,  1); lua_pop(L, 1);
// Optional: Include coroutines and debug support
// luaL_requiref(L, "coroutine",  luaopen_coroutine,  1); lua_pop(L, 1);
// luaL_requiref(L, "debug",      luaopen_debug,  1);     lua_pop(L, 1);
  
lua_register(L, "cli_prompt", l_cli_prompt);
lua_register(L, "get_line", l_get_line);
luahal_register(L);  // Registers rd, wr, pin, dr, dw, etc
  
embedded_cli_init(&cli, "> ", put_char, NULL);
printf("Finished setup\r\n");
  
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
  
while (1)
{
  /* USER CODE END WHILE */
```

4. Run `make`, flash, and test!

## Running a Lua REPL written in Lua on a Raspberry Pi Pico

Sample project [`lua_in_lua_on_rpi_pico`](lua_in_lua_on_rpi_pico).

1. Clone [pico-sdk](https://github.com/raspberrypi/pico-sdk) and pull all submodules.

```bash
cd #desired folder location
git clone https://github.com/raspberrypi/pico-sdk
git clone --recurse-submodules
```

2. Create a project folder for your source and build files.

```bash
mkdir my_project
cd my_project
```

3. Create a `CMakeLists.txt` like [this one](lua_in_lua_on_rpi_pico/CMakeLists.txt) for your application and add it to your project folder. If you're copying that file, don't forget the change 2, 8, 15, 22, and 25 to the correct paths for your files!

4. Add [`main.c`](lua_in_lua_on_rpi_pico/main.c) and, if desired, a small [HAL](lua_in_lua_on_rpi_pico/lua_hal_RP2040.c).

5. Set up the CMake build directory.

```bash
cmake -S . -B build
```

6. Build your application

```bash
cmake --build build --target rp2040_and_HAL
```

6. Flash the Pi Pico by holding BOOTSEL while plugging the USB cable into your computer. Drag and drop the `rp2040_and_HAL.uf2` file from the `build` folder to the USB mass storage device that should have appeared when you connected the Pi Pico. The Pico should restart and begin running the application as soon as the file transfer finishes.