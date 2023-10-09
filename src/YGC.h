#ifndef YGC_H
#define YGC_H

#include "lj_obj.h"

/* windows and c89 support nogc but not bggc */
#if !defined(LUA_USE_WINDOWS)
#include <pthread.h>
#endif

/* lgc.c */
#define Y_NOGCBIT 0x80 /* object not to be collected */
#define testbits(x,m)       ((x) & (m))
#define Y_isnogc(x) testbits((x).marked, Y_NOGCBIT)

/* lbaselib.c */
int nogc (lua_State *L);
int bggc (lua_State *L);

/* lstate.c */
void Y_initstate (lua_State *L);

/* lgc.c */
typedef struct Y_bgjob Y_bgjob;
Y_bgjob* Y_createbgjob (lua_State *L);
void Y_submitbgjob (lua_State *L, Y_bgjob *j);
void Y_trybgfree (lua_State*, GCobj *, Y_bgjob*, void(*)(global_State*, GCobj*));

/* lbaselib.c */
#define Y_BASEFUNCS \
{"nogc", nogc}, \
{"bggc", bggc}


#endif

