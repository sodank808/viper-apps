/* nhlua.h - Viper: the top Makefile generates this upstream; here it names
   the Lua that's in ../../lua (an include folder of the app's build) */
#include "lua.h"
ATTRNORETURN LUA_API int   (lua_error) (lua_State *L) NORETURN;
#include "lualib.h"
#include "lauxlib.h"
/*nhlua.h*/
