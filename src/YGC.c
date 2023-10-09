#include "YGC.h"

#include <string.h>
#include <stdio.h>

#include "lj_func.h"
#include "lj_tab.h"
#include "lj_str.h"
#include "lj_gc.h"
#include "lj_state.h"
#include "lj_meta.h"

//#include "lauxlib.h"

#define Y_NOGCCCLOSE 0
#define Y_NOGCOPEN   1
#define Y_NOGCCOUNT  2
#define Y_NOGCLEN    3

#define cast(t, exp)        ((t)(exp))
#define setbits(x,m)        ((x) |= (m))
#define l_setbit(x,b)       setbits(x, (b))
#define Y_makenogc(x)       l_setbit((x)->marked, Y_NOGCBIT)

#define cast_byte(i)	    cast(uint8_t, (i))
#define resetbits(x,m)      ((x) &= cast(uint8_t, ~(m)))
#define resetbit(x,b)       resetbits(x, (b))
#define Y_clearnogc(x) resetbit((x).marked, Y_NOGCBIT)
#define isdummy(t)      ((t)->lastfree == NULL)

static void Y_linkrevert (global_State *g, GCobj *o);
static void Y_closeupvalue (lua_State *L, GCupval *u);
static void Y_reallymarkobject (lua_State *L, GCobj *o, int b);
static void Y_traverseproto (lua_State *L, GCproto *f, int b);
static void Y_traverseLclosure (lua_State *L, GCfunc *cl, int b);
static void Y_traversetable (lua_State *L, GCtab *h, int b);
static int  Y_isweaktable (lua_State *L, const struct GCtab *h);

#define Y_markobject(L, t, b) { Y_reallymarkobject(L, obj2gco(t), b); }
#define Y_markobjectN(L, t, b) { if (t) Y_markobject(L, t, b); }
#define iscollectable(v) tvisgcv(v)
#define Y_valis(v, b) (iscollectable(v) && (b ? !Y_isnogc(gcval(v)->gch) : Y_isnogc(gcval(v)->gch)))
#define Y_markvalue(L, o, b) { if (Y_valis(o, b)) Y_reallymarkobject(L, gcval(o), b); }
#define Y_maskcolors (~(LJ_GC_COLORS))
#define Y_makeblack(x) \
    (x.marked = cast_byte((x.marked & Y_maskcolors) | cast(uint8_t, LJ_GC_BLACK)))
#define Y_resetobject(g,o) \
    { Y_clearnogc(o->gch); Y_makeblack(o->gch); Y_linkrevert(g, o);  }


static void Y_linkrevert (global_State *g, GCobj *o) {
  GCRef p = g->Y_nogc;
  GCobj *curr;
  while ((curr = gcref(p)) != NULL) {
    if (curr == o) {
      setgcrefr(p, curr->gch.nextgc);
      setgcrefr(curr->gch.nextgc, g->gc.root);
      setgcref(g->gc.root, curr);
    }
    p = curr->gch.nextgc;
  }
}

extern void unlinkuv(global_State *g, GCupval *uv);
static void Y_closeupvalue (lua_State *L, GCupval *u) {
  GCupval *uv;
  global_State *g = G(L);
  while (gcref(L->openupval) != NULL && 
      (uv = gco2uv(gcref(L->openupval)))) {
    if (u == uv) {
      unlinkuv(g, uv);
      //lj_gc_closeuv(g, uv);
      break;
    }
  }
  return;
}

static void Y_reallymarkobject (lua_State *L, GCobj *o, int b) {
  global_State *g = G(L);
  int gct = o->gch.gct;
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    if (Y_isweaktable(L, gco2tab(o))) {
      lj_assertG(0, "Not support weak tables");
      return;
    }
    if (b) {
      Y_makenogc(&o->gch);
    } else {
      Y_resetobject(g, o);
    }
    Y_traversetable(L, gco2tab(o), b);
  } else if (LJ_LIKELY(gct == ~LJ_TSTR)) {
    lu_mem mem = lj_str_size(gco2str(o)->len);
    if (b) {
      Y_makenogc(&o->gch);
    } else {
      Y_resetobject(g, o);
    }
    g->Y_GCmemnogc += (b ? mem : -mem);
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    GCfunc *fn = gco2func(o);
    if (!isluafunc(fn)) {
      lj_assertG(0, "Not support cclusor");
      return;
    }
    if (b) {
      Y_makenogc(&o->gch);
    } else {
      Y_resetobject(g, o);
    }
    Y_traverseLclosure(L,  gco2func(o), b);
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    if (b) {
      Y_makenogc(&o->gch);
    } else {
      Y_resetobject(g, o);
    }
    Y_traverseproto(L, gco2pt(o), b);
  } else if (LJ_LIKELY(gct == ~LJ_TUDATA ||
      gct == ~LJ_TTHREAD) ) {
    lj_assertG(0, "Not support userdata thread cclusor");
  } else {
    lj_assertG(0, "Not support others");
  }
}

static void Y_traverseproto (lua_State *L, GCproto *pt, int b) {
  ptrdiff_t i;
  Y_markobjectN(L, proto_chunkname(pt), b);

  for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++)  // Mark collectable consts
    Y_markobjectN(L, proto_kgc(pt, i), b);

#if LJ_HASJIT
  // if (pt->trace) gc_marktrace(g, pt->trace); // TODO
#endif
  lu_mem mem = pt->sizept;
  G(L)->Y_GCmemnogc += (b ? mem : -mem);
}

#define gc_marktv(g, tv) \
  { lj_assertG(!tvisgcv(tv) || (~itype(tv) == gcval(tv)->gch.gct), \
	       "TValue and GC type mismatch"); \
    if (tviswhite(tv)) gc_mark(g, gcV(tv)); }

static void Y_traverseLclosure (lua_State *L, GCfunc *fn, int b) {
  int i;
  Y_markobjectN(L, funcproto(fn), b);
  for (i = 0; i < fn->l.nupvalues; i++) {
    GCupval *uv = &gcref(fn->l.uvptr[i])->uv;
    TValue *v = mref(uv->v, TValue);
    if (b && uv->closed == 0) {
      Y_closeupvalue(L, uv);
    }
    Y_markvalue(L, v, b);
  }
  lu_mem mem = sizeLfunc((MSize)fn->l.nupvalues);
  G(L)->Y_GCmemnogc += (b ? mem : -mem);
}

static void Y_traversetable (lua_State *L, GCtab *h, int b) {
  Y_markobjectN(L, tabref(h->metatable), b);
  MSize i, asize = h->asize;
  for (i = 0; i < asize; i++) {
	TValue *tv = arrayslot(h, i);
    Y_markvalue(L, tv, b);
  }
  if (h->hmask > 0) {
    Node *node = noderef(h->node);
    MSize i, hmask = h->hmask;
    for (i = 0; i <= hmask; i++) {
	  Node *n = &node[i];
      if (!tvisnil(&n->val)) {
        Y_markvalue(L, &n->key, b);
        Y_markvalue(L, &n->val, b);
      }
    }
  }
  lu_mem mem = sizeof(GCtab) + sizeof(TValue) * h->asize +
			   (h->hmask ? sizeof(Node) * (h->hmask + 1) : 0);
  G(L)->Y_GCmemnogc += (b ? mem : -mem);
}

static int Y_isweaktable (lua_State *L, const struct GCtab *t) {
  cTValue *mode;
  global_State *g = G(L);
  GCtab *mt = tabref(t->metatable);
  mode = lj_meta_fastg(g, mt, MM_mode);
  if (mode && tvisstr(mode)) {
    const char *modestr = strVdata(mode);
    int c;
    while ((c = *modestr++)) {
      if (c == 'k' || c == 'v')
        return 1;
    }
  }
  return 0;
}

static const struct GCtab* Y_opttable (lua_State *L, int arg) {
  if (lua_isnoneornil(L, arg) || lua_type(L, arg) != LUA_TTABLE) {
    return NULL;
  }
  return cast(GCtab*, lua_topointer(L, arg));
}

static int Y_nogc (lua_State *L, int what, const struct GCtab *h) {
  int res = 0;
  global_State *g = G(L);
  switch (what) {
    case Y_NOGCCCLOSE: {
      if (!h) {
        lj_assertG(0, "Missing a table object");
        break;
      }
      Y_markobject(L, h, Y_NOGCCCLOSE);
      break;
    }
    case Y_NOGCOPEN: {
      if (!h) {
        lj_assertG(0, "Missing a table object");
        break;
      }
      Y_markobject(L, h, Y_NOGCOPEN);
      break;
    }
    case Y_NOGCCOUNT: {
      //res = (int)(g->Y_GCmemnogc >> 10);
      res = (int)(g->Y_GCmemnogc);
      break;
    }
    case Y_NOGCLEN: {
      GCRef p = g->Y_nogc;
      GCobj *curr;
      while ((curr = gcref(p)) != NULL) {
        res ++;
        p = curr->gch.nextgc;
      }
      break;
    }
    default: res = -1;
  }
  return res;
}

extern int luaL_checkoption(lua_State *L, int idx, const char *def,
				const char *const lst[]);

int nogc (lua_State *L) {
  static const char* const opts[] = {"close", "open", "count",
    "len", NULL};
  static const int optsum[] = {Y_NOGCCCLOSE, Y_NOGCOPEN, Y_NOGCCOUNT,
    Y_NOGCLEN};
  int o = optsum[luaL_checkoption(L, 1, "count", opts)];
  const struct GCtab *ex = Y_opttable(L, 2);
  int res = Y_nogc(L, o, ex);
  switch (o) {
    case Y_NOGCCOUNT: {
      lua_pushnumber(L, (lua_Number)res + ((lua_Number)res/1024));
      return 1;
    }
    case Y_NOGCLEN: {
      lua_pushinteger(L, res);
      return 1;
    }
    default: return 0;
  }
  return 0;
}

/* ------------------------ Background Garbage Collect ------------------------ */

#define Y_BGGCCLOSE 0
#define Y_BGGCOPEN 1
#define Y_BGGCISRUNNING 2

static void Y_luaM_free_ (lua_State *L, void *block, size_t osize);
static void *Y_luaM_malloc (lua_State *L, size_t nsize);
static void Y_luaF_freeproto (lua_State *L, GCproto *f);
static void Y_luaH_free (lua_State *L, GCtab *t);
static size_t Y_linkbgjob (global_State *g, Y_bgjob *j, GCobj *o);
//static void Y_upvdeccount (lua_State *L, LClosure *cl);
static void Y_freeobj (lua_State *L, GCobj *o);
static void *Y_bgProcessJobs (void *arg);

#define Y_luaM_freemem(L, b, s) Y_luaM_free_(L, (b), (s))
#define Y_luaM_free(L, b) Y_luaM_free_(L, (b), sizeof(*(b)))
#define Y_luaM_freearray(L, b, n) Y_luaM_free_(L, (b), (n)*sizeof(*(b)))
#define Y_luaM_new(L, t) cast(t*, Y_luaM_malloc(L, sizeof(t)))

static void Y_luaM_free_ (lua_State *L, void *block, size_t osize) {
  //global_State *g = G(L);
  //(*g->frealloc)(g->ud, block, osize, 0);
  free(block);
}

static void *Y_luaM_malloc (lua_State *L, size_t nsize) {
  //global_State *g = G(L);
  //void *newblock = (*g->frealloc)(g->ud, NULL, 0, nsize);
  void *newblock = (void*)malloc(nsize);
  return newblock;
}

static void Y_luaF_freeproto (lua_State *L, GCproto *pt) {
  global_State *g = G(L);
  lj_mem_free(g, pt, pt->sizept);
}

static void Y_luaH_free (lua_State *L, GCtab *t) {
  global_State *g = G(L);
  if (t->hmask > 0)
    lj_mem_freevec(g, noderef(t->node), t->hmask+1, Node);
  if (t->asize > 0 && LJ_MAX_COLOSIZE != 0 && t->colo <= 0)
    lj_mem_freevec(g, tvref(t->array), t->asize, TValue);
  if (LJ_MAX_COLOSIZE != 0 && t->colo)
    lj_mem_free(g, t, sizetabcolo((uint32_t)t->colo & 0x7f));
  else
    lj_mem_freet(g, t);
}

static Y_bgjob *Y_jobs = NULL;
struct Y_bgjob {
  Y_bgjob *next;
  GCRef    Y_bggc; // GCobj *Y_bggc;
};

/* link GCobj to a background job 
  and return the size of the GCobj 
  that will be released */
static size_t Y_linkbgjob (global_State *g, Y_bgjob *j, GCobj *o) {
  setgcrefr(o->gch.nextgc, j->Y_bggc);
  setgcref(j->Y_bggc, o);

  size_t osize = 0;
  int gct = o->gch.gct;
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    GCtab *t = gco2tab(o);
    osize += sizeof(GCtab) + sizeof(TValue) * t->asize +
			   (t->hmask ? sizeof(Node) * (t->hmask + 1) : 0);
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    GCfunc *fn = gco2func(o);
    osize += isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
			   sizeCfunc((MSize)fn->c.nupvalues);
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    GCproto *pt = gco2pt(o);
    osize += pt->sizept;
  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    //lua_State *th = gco2th(o);
    //osize += sizeof(lua_State) + sizeof(TValue) * th->stacksize;
  } else if (LJ_LIKELY(gct == ~LJ_TSTR)) {
	GCstr *s = gco2str(o);
    osize += lj_str_size(s->len);
  } else if (gct == ~LJ_TCDATA) {
    osize += sizeudata(gco2ud(o));
  } else {
    lj_assertG(0, "Y_linkbgjob gct other");
  }
  return osize;
}

/*
static void Y_upvdeccount (lua_State *L, LClosure *cl) {
  int i;
  for (i = 0; i < cl->nupvalues; i++) {
    GCupval *uv = cl->upvals[i];
    if (uv)
      luaC_upvdeccount(L, uv);
  }
}
*/
static uint64_t a_table_t = 0;
static uint64_t a_proto_t = 0;
static uint64_t a_thread_t = 0;
static uint64_t a_func_t = 0;
static uint64_t a_str_t = 0;
static uint64_t a_udata_t = 0;
static uint64_t a_other_t = 0;

static uint64_t f_table_t = 0;
static uint64_t f_proto_t = 0;
static uint64_t f_func_t = 0;
static uint64_t f_str_t = 0;
static uint64_t f_udata_t = 0;
static uint64_t f_other_t = 0;

static void print_state() {
  printf("alloc time: %ld table: %ld proto: %ld func: %ld str: %ld udata: %ld other: %ld thr: %ld\n", 
        time(0), a_table_t, a_proto_t, a_func_t, a_str_t, a_udata_t, a_other_t, a_thread_t);
  printf("free  time: %ld table: %ld proto: %ld func: %ld str: %ld udata: %ld other: %ld\n", 
        time(0), f_table_t, f_proto_t, f_func_t, f_str_t, f_udata_t, f_other_t);
}

static void Y_freeobj (lua_State *L, GCobj *o) {
  /* threads are released in the main thread */
  global_State *g = G(L);
  int gct = o->gch.gct;
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    Y_luaH_free(L, gco2tab(o));
    f_table_t++;
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    Y_luaF_freeproto(L, gco2pt(o));
    f_proto_t++;
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    GCfunc *fn = gco2func(o);
    MSize size = isluafunc(fn) ? sizeLfunc((MSize)fn->l.nupvalues) :
      		       sizeCfunc((MSize)fn->c.nupvalues);
    lj_mem_free(g, fn, size);
    f_func_t++;
  } else if (LJ_LIKELY(gct == ~LJ_TSTR)) {
	GCstr *s = gco2str(o);
    g->str.num--;
    lj_mem_free(g, s, lj_str_size(s->len));
    f_str_t++;
  } else if (LJ_LIKELY(gct == ~LJ_TUDATA)) {
    GCudata *u = gco2ud(o);
    lj_mem_free(g, u, sizeudata(u));
    f_udata_t++;
  } else {
    f_other_t++;
    lj_assertG(0, "Y_freeobj gct other");
  }
}

void *Y_bgProcessJobs (void *arg) {
  lua_State *L= cast(lua_State*, arg);
  global_State *g = G(L);
#if !defined(LUA_USE_WINDOWS)
  pthread_mutex_lock(&g->Y_bgmutex);
  while (1) {
    Y_bgjob **p = &Y_jobs;
    if (*p == NULL) {
      pthread_cond_wait(&g->Y_bgcond, &g->Y_bgmutex);
      continue;
    }
    Y_bgjob *curr = *p;
    *p = curr->next;
    print_state();
    pthread_mutex_unlock(&g->Y_bgmutex);
    GCRef r = (curr->Y_bggc);
    GCobj *o;
    while ((o = gcref(r)) != NULL) {
      r = o->gch.nextgc;
      Y_freeobj(L, o);
    }
    Y_luaM_free(L, curr);
    pthread_mutex_lock(&g->Y_bgmutex);
  }
#endif
  return NULL;
}

Y_bgjob *Y_createbgjob (lua_State *L) {
  if (!G(L)->Y_bgrunning) return NULL;
  Y_bgjob *j = Y_luaM_new(L, Y_bgjob);
  j->next = NULL;
  setgcrefnull(j->Y_bggc);
  return j;
}

void Y_submitbgjob (lua_State *L, Y_bgjob *j) {
  global_State *g = G(L);
  if (!g->Y_bgrunning) return;
#if !defined(LUA_USE_WINDOWS)
  pthread_mutex_lock(&g->Y_bgmutex);
  j->next = Y_jobs;
  Y_jobs = j;
  pthread_cond_signal(&g->Y_bgcond);
  pthread_mutex_unlock(&g->Y_bgmutex);
#endif
}

/*
static void state_free(GCobj *o) {
  static uint64_t c = 0;
  c++;
  int gct = o->gch.gct;
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    a_table_t++;
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    a_func_t++;
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    a_proto_t++;
  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    a_thread_t++;
  } else if (LJ_LIKELY(gct == ~LJ_TUDATA)) {
    a_udata_t++;
  } else if (LJ_LIKELY(gct == ~LJ_TSTR)) {
    a_str_t++;
  } else {
    a_other_t++;
  }
  if (c > 1000) {
    c = 0;
    printf("alloc time: %ld table: %ld proto: %ld func: %ld str: %ld udata: %ld other: %ld thr: %ld\n", 
          time(0), a_table_t, a_proto_t, a_func_t, a_str_t, a_udata_t, a_other_t, a_thread_t);
  }
  return;
}
*/

void Y_trybgfree (lua_State *L, GCobj *o, Y_bgjob *j, void(*fgfreeobj)(global_State*, GCobj*)) {
  global_State *g = G(L);
  if (!g->Y_bgrunning) {
    fgfreeobj(g, o);
    return;
  }
  size_t osize = 0;
  int gct = o->gch.gct;
#if 0
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    osize = Y_linkbgjob(g, j, o);
    a_table_t++;
  } else if (LJ_LIKELY(gct == ~LJ_TFUNC)) {
    osize = Y_linkbgjob(g, j, o);
    a_func_t++;
  } else if (LJ_LIKELY(gct == ~LJ_TPROTO)) {
    // CL: Y_upvdeccount(L, gco2lcl(o));
    osize = Y_linkbgjob(g, j, o);
    a_proto_t++;
  } else if (LJ_LIKELY(gct == ~LJ_TTHREAD)) {
    lj_state_free(g, gco2th(o));
    a_thread_t++;
  } else if (LJ_LIKELY(gct == ~LJ_TUDATA)) {
    osize = Y_linkbgjob(g, j, o);
    a_udata_t++;
  } else if (LJ_LIKELY(gct == ~LJ_TSTR)) {
    osize = Y_linkbgjob(g, j, o);
    a_str_t++;
  } else {
    a_other_t++;
    lj_assertG(0, "Y_trybgfree gct other");
  }
  g->gc.debt -= osize;
#else
  if (LJ_LIKELY(gct == ~LJ_TTAB)) {
    osize = Y_linkbgjob(g, j, o);
    g->gc.debt -= osize;
    a_table_t++;
  } else {
    fgfreeobj(g, o);
  }
#endif
}

void Y_initstate (lua_State *L) {
  global_State *g = G(L);
  setgcrefnull(g->Y_nogc);
  g->Y_GCmemnogc = 0;
  g->Y_bgrunning = 0;
#if !defined(LUA_USE_WINDOWS)
  pthread_mutex_init(&g->Y_bgmutex, NULL);
  pthread_cond_init(&g->Y_bgcond, NULL);
  /* fixme: check return value */
  pthread_create(&g->Y_bgthread, NULL, Y_bgProcessJobs, cast(void*, L));
#endif
}

static int Y_bggc (lua_State *L, int what) {
  int res = 0;
  global_State *g = G(L);
  switch (what) {
    case Y_BGGCCLOSE: {
      g->Y_bgrunning = 0;
      break;
    }
    case Y_BGGCOPEN: {
      g->Y_bgrunning = 1;
      break;
    }
    case Y_BGGCISRUNNING: {
      res = g->Y_bgrunning;
      break;
    }
    default: res = -1;
  }
  return res;
}

#if defined(LUA_USE_WINDOWS)
int bggc (lua_State *L) { luaL_error(L, "Not support for windows"); }
#else
int bggc (lua_State *L) {
  static const char* const opts[] = {"close", "open", "isrunning", NULL};
  static const int optsum[] = {Y_BGGCCLOSE, Y_BGGCOPEN, Y_BGGCISRUNNING};
  int o = optsum[luaL_checkoption(L, 1, "isrunning", opts)];
  int res = Y_bggc(L, o);
  if (o == Y_BGGCISRUNNING) {
    lua_pushinteger(L, res);
    return 1;
  }
  return 0;
}
#endif

