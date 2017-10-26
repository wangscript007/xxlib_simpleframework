extern "C"
{
#include "../lua/lapi.c"
#include "../lua/lauxlib.c"
#include "../lua/lbaselib.c"
#include "../lua/lbitlib.c"
#include "../lua/lcode.c"
#include "../lua/lcorolib.c"

//#include "../lua/lctype.c"
#include "lctype.c"				// 支持了中文变量名

#include "../lua/ldblib.c"
#include "../lua/ldebug.c"
#include "../lua/ldo.c"

//#include "../lua/ldump.c"
#include "ldump.c"				// 支持了 32/64 位 bytecode 的跨越

#include "../lua/lfunc.c"
#include "../lua/lgc.c"
#include "../lua/linit.c"
#include "../lua/liolib.c"
#include "../lua/llex.c"
#include "../lua/lmathlib.c"
#include "../lua/lmem.c"
#include "../lua/lobject.c"
#include "../lua/lopcodes.c"
#include "../lua/loslib.c"
#include "../lua/lparser.c"
#include "../lua/lstate.c"
#include "../lua/lstring.c"
#include "../lua/lstrlib.c"
#include "../lua/ltable.c"
#include "../lua/ltablib.c"
#include "../lua/ltests.c"
#include "../lua/ltm.c"

//#include "../lua/lua.c"		// 这是命令行版lua, 并不需要

//#include "../lua/lundump.c"
#include "lundump.c"			// 支持了 32/64 位 bytecode 的跨越

#include "../lua/lutf8lib.c"
#include "../lua/lvm.c"
#include "../lua/lzio.c"

// 这个内含 windows.h 之 include, 故放到最下面 避免污染上面的函数名
#include "../lua/loadlib.c"				

}

void SetUtf8Console()
{
#ifdef _WIN32
	SetConsoleOutputCP(65001);
#endif
}
