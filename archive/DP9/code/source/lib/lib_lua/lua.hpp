// lua.hpp — C++ wrapper for Lua C headers
#ifndef LUA_32BITS
#define LUA_32BITS 1
#endif

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
