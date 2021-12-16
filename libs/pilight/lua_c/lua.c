/*
	Copyright (C) 2013 - 2016 CurlyMo

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <limits.h>
#include <assert.h>
#include <setjmp.h>

#ifndef _WIN32
	#include <libgen.h>
	#include <dirent.h>
	#include <unistd.h>
#endif

#include "lua.h"
#include "lualibrary.h"

#include "../core/log.h"
#include "../core/json.h"
#include "../core/mem.h"
#include "../core/common.h"
#include "table.h"

#ifdef PILIGHT_UNITTEST
static struct info_t {
	char *module;
	char *function;
} info = {
	NULL, NULL
};

struct coverage_t {
	char *file;
	int *lstat;
	int *fstat;
	char **functions;
	char *module;
	int maxline;

	int nrlstat;
	int nrfstat;
	int nrfunctions;
} **coverage = NULL;
static int nrcoverage = 0;

static int covpause = 0;
static const char *covfile = NULL;
#endif

static int init = 0;
/*
 * Last state is a global state for global
 * garbage collection on pilight shutdown.
 */
static struct lua_state_t lua_state[NRLUASTATES+1];
static uv_sem_t sem_used_states;
static struct plua_module_t *modules = NULL;

static int plua_metatable_index(lua_State *L, struct plua_metatable_t *node);
static int plua_metatable_pairs(lua_State *, struct plua_metatable_t *node);
static int plua_metatable_ipairs(lua_State *L, struct plua_metatable_t *node);
static int plua_metatable_next(lua_State *L, struct plua_metatable_t *node);
static int plua_metatable_call(lua_State *L, struct plua_metatable_t *node);
static int plua_metatable_newindex(lua_State *L, struct plua_metatable_t *node);
static int plua_metatable_metatable(lua_State *L, struct plua_metatable_t *node);
static int plua_metatable__index(lua_State *L);
static int plua_metatable__gc(lua_State *L);
static int plua_metatable__pairs(lua_State *L);
static int plua_metatable__ipairs(lua_State *L);
static int plua_metatable__next(lua_State *L);
static int plua_metatable__call(lua_State *L);
static int plua_metatable__newindex(lua_State *L);
static int plua_metatable__metatable(lua_State *L);
static int plua_metatable___index(lua_State *L);
static int plua_metatable___gc(lua_State *L);
static int plua_metatable___pairs(lua_State *L);
static int plua_metatable___ipairs(lua_State *L);
static int plua_metatable___next(lua_State *L);
static int plua_metatable___call(lua_State *L);
static int plua_metatable___newindex(lua_State *L);
static int plua_metatable___metatable(lua_State *L);

/* LCOV_EXCL_START */
void plua_dump_table(lua_State *L) {
	lua_pushnil(L);
	while(lua_next(L, -2)) {
		lua_pushvalue(L, -2);
		switch(lua_type(L, -2)) {
			case LUA_TSTRING:
			case LUA_TNUMBER:
				printf("%s = %s\n", lua_tostring(L, -1), lua_tostring(L, -2));
				break;
			case LUA_TFUNCTION:
				printf("%s()\n", lua_tostring(L, -1));
				break;
			case LUA_TTABLE: {
				printf("%s:", lua_tostring(L, -1));
				lua_pushvalue(L, -2);
				plua_dump_table(L);
				lua_pop(L, 1);
				printf("\n");
				break;
			}
			case LUA_TBOOLEAN:
				printf("%s = %s\n", lua_tostring(L, -1), (lua_toboolean(L, -2) ? "true" : "false"));
				break;
			default:
				printf("%s = <unparsed type>\n", lua_tostring(L, -1));
		}
		lua_pop(L, 2);
	}
}

void plua_stack_dump(lua_State *L) {
	int i = 0;
	int top = lua_gettop(L);
	for(i = 1; i <= top; i++) {  /* repeat for each level */
		int t = lua_type(L, i);
		switch(t) {
			case LUA_TSTRING:  /* strings */
				printf("%d: '%s'", i, lua_tostring(L, i));
			break;
			case LUA_TBOOLEAN:  /* booleans */
				printf("%d: %s", i, lua_toboolean(L, i) ? "true" : "false");
			break;
			case LUA_TNUMBER:  /* numbers */
				printf("%d: %g", i, lua_tonumber(L, i));
			break;
			default:  /* other values */
				printf("%d: %s", i, lua_typename(L, t));
			break;
		}
		printf("  ");  /* put a separator */
	}
	printf("\n");  /* end the listing */
}
/* LCOV_EXCL_STOP */

/*
 * Backported from lua5.2
 */
static int pairsmeta (lua_State *L, const char *method, int iszero,
                      lua_CFunction iter) {
  struct lua_state_t *state = plua_get_current_state(L);

  if (!luaL_getmetafield(L, 1, method)) {  /* no metamethod? */
    luaL_checktype(L, 1, LUA_TTABLE);  /* argument must be a table */
    lua_pushcfunction(L, iter);  /* will return generator, */
    lua_pushvalue(L, 1);  /* state, */
    if (iszero) lua_pushinteger(L, 0);  /* and initial value */
    else lua_pushnil(L);
  }
  else {
    lua_pushvalue(L, 1);  /* argument 'self' to metamethod */
    plua_pcall(L, state->module->file, 1, 3); /* get 3 values from metamethod */
  }
  return 3;
}

static int luaB_next (lua_State *L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 2);  /* create a 2nd argument if there isn't one */

  if (lua_next(L, 1))
    return 2;
  else {
    lua_pushnil(L);
    return 1;
  }
}

static int luaB_pairs (lua_State *L) {
  return pairsmeta(L, "__pairs", 0, luaB_next);
}

static int ipairsaux (lua_State *L) {
  int i = luaL_checkint(L, 2);
  luaL_checktype(L, 1, LUA_TTABLE);
  i++;  /* next value */
  lua_pushinteger(L, i);
  lua_rawgeti(L, 1, i);
  return (lua_isnil(L, -1)) ? 1 : 2;
}

static int luaB_ipairs (lua_State *L) {
  return pairsmeta(L, "__ipairs", 1, ipairsaux);
}

int plua_namespace(struct plua_module_t *module, char *p) {
	switch(module->type) {
#ifdef PILIGHT_UNITTEST
		case UNITTEST: {
			sprintf(p, "unittest.%s", module->name);
			return 0;
		} break;
#endif
		case FUNCTION: {
			sprintf(p, "function.%s", module->name);
			return 0;
		} break;
		case OPERATOR: {
			sprintf(p, "operator.%s", module->name);
			return 0;
		} break;
		case ACTION: {
			sprintf(p, "action.%s", module->name);
			return 0;
		} break;
		case STORAGE: {
			sprintf(p, "storage.%s", module->name);
			return 0;
		} break;
		case HARDWARE: {
			sprintf(p, "hardware.%s", module->name);
			return 0;
		} break;
		case PROTOCOL: {
			sprintf(p, "protocol.%s", module->name);
			return 0;
		} break;
	}
	return -1;
}

struct lua_state_t *plua_get_module(lua_State *L, char *namespace, char *module) {
	struct lua_state_t *state = NULL;
	int match = 0;

	state = plua_get_current_state(L);

	if(state == NULL) {
		return NULL;
	}

	if((L = state->L) == NULL) {
		assert(plua_check_stack(L, 0) == 0);
		return NULL;
	}

	char name[255], *p = name;
	memset(name, '\0', 255);

	sprintf(p, "%s.%s", namespace, module);
	lua_getglobal(L, name);

	if(lua_isnil(L, -1) != 0) {
		lua_pop(L, -1);
		assert(plua_check_stack(L, 0) == 0);
		return NULL;
	}
	if(lua_istable(L, -1) != 0) {
		struct plua_module_t *tmp = plua_get_modules();
		while(tmp) {
			if(strcmp(module, tmp->name) == 0) {
				state->module = tmp;
				match = 1;
				break;
			}
			tmp = tmp->next;
		}
		if(match == 1) {
			return state;
		}
	}

	lua_pop(L, -1);

	assert(plua_check_stack(L, 0) == 0);

	return NULL;
}

static int plua_metatable_len(lua_State *L, struct plua_metatable_t *node) {
	if(node == NULL) {
		logprintf(LOG_ERR, "internal error: table object not passed");
		return 0;
	}

	uv_mutex_lock(&node->lock);
	lua_pushnumber(L, node->nrvar);
	uv_mutex_unlock(&node->lock);

	return 1;
}

static int plua_metatable__len(lua_State *L) {
	struct plua_metatable_t *node = (void *)lua_topointer(L, lua_upvalueindex(1));

	return plua_metatable_len(L, node);
}

static int plua_metatable___len(lua_State *L) {
	struct plua_interface_t *interface = (void *)lua_topointer(L, lua_upvalueindex(1));
	struct plua_metatable_t *node = interface->table;

	return plua_metatable_len(L, node);
}

static void plua_metatable_unref(void *ptr) {
	struct plua_metatable_t *table = ptr;
	plua_metatable_free(table);
}

static int plua_metatable_push(lua_State *L, struct plua_metatable_t *node) {
	char buf[128] = { '\0' }, *p = buf;
	char *error = "string, number, table, boolean or nil expected, got %s";

	if(node == NULL) {
		logprintf(LOG_ERR, "internal error: table object not passed or already freed");
		return 0;
	}

	if(lua_gettop(L) != 1) {
		pluaL_error(L, "metatable push requires 0 arguments, %d given", lua_gettop(L));
	}

	sprintf(p, error, lua_typename(L, lua_type(L, -1)));

	luaL_argcheck(L,
		((lua_type(L, -1) == LUA_TSTRING) || (lua_type(L, -1) == LUA_TNUMBER) ||
		 (lua_type(L, -1) == LUA_TTABLE || (lua_type(L, -1) == LUA_TBOOLEAN))),
		1, buf);

	int max = 0, x = 0;

	uv_mutex_lock(&node->lock);
	for(x=0;x<node->nrvar;x++) {
		uv_mutex_lock(&node->table[x].lock);
		if(node->table[x].key.type_ == LUA_TSTRING) {
			uv_mutex_unlock(&node->table[x].lock);
			uv_mutex_unlock(&node->lock);
			pluaL_error(L, "metatable push only works on numeric arrays");
		} else {
			if(node->table[x].key.number_ > max) {
				max = node->table[x].key.number_;
			}
		}
		uv_mutex_unlock(&node->table[x].lock);
	}

	int idx = node->nrvar;
	if((node->table = REALLOC(node->table, sizeof(*node->table)*(idx+1))) == NULL) {
		OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
	}
	uv_mutex_init(&node->table[idx].lock);
	switch(lua_type(L, -1)) {
		case LUA_TBOOLEAN: {
			node->table[idx].val.number_ = lua_toboolean(L, -1);
			node->table[idx].val.type_ = LUA_TBOOLEAN;
		} break;
		case LUA_TNUMBER: {
			node->table[idx].val.number_ = lua_tonumber(L, -1);
			node->table[idx].val.type_ = LUA_TNUMBER;
		} break;
		case LUA_TSTRING: {
			if((node->table[idx].val.string_ = STRDUP((char *)lua_tostring(L, -1))) == NULL) {
				OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
			};
			node->table[idx].val.type_ = LUA_TSTRING;
		} break;
		case LUA_TTABLE: {
			int is_metatable = 0;

			node->table[idx].val.void_ = NULL;

			node->table[idx].val.type_ = LUA_TTABLE;
			if((is_metatable = lua_getmetatable(L, -1)) == 1) {
				lua_remove(L, -1);
				if(luaL_getmetafield(L, -1, "__call")) {
					if(plua_pcall(L, __FILE__, 1, 1) == 0) {
						if(lua_type(L, -1) == LUA_TLIGHTUSERDATA) {
							struct plua_metatable_t *table = lua_touserdata(L, -1);
							plua_metatable_clone(&table, (struct plua_metatable_t **)&node->table[idx].val.void_);
						} else {
							logprintf(LOG_ERR, "metatable metafield __call does not return userdata");
						}
					}
				} else {
					logprintf(LOG_ERR, "metatable does not the call metafield");
				}
			} else {
				plua_metatable_init((struct plua_metatable_t **)&node->table[idx].val.void_);
				lua_pushnil(L);
				while(lua_next(L, -2) != 0) {
					plua_metatable_parse_set(L, node->table[idx].val.void_);
					lua_pop(L, 1);
				}
			}
		} break;
	}

	node->table[idx].key.number_ = max+1;
	node->table[idx].key.type_ = LUA_TNUMBER;

	node->nrvar++;
	uv_mutex_unlock(&node->lock);

	return 1;
}

static int plua_metatable__push(lua_State *L) {
	struct plua_metatable_t *node = (void *)lua_topointer(L, lua_upvalueindex(1));

	return plua_metatable_push(L, node);
}

static int plua_metatable___push(lua_State *L) {
	struct plua_interface_t *interface = (void *)lua_topointer(L, lua_upvalueindex(1));
	struct plua_metatable_t *node = interface->table;

	return plua_metatable_push(L, node);
}

static int plua_metatable_pop(lua_State *L, struct plua_metatable_t *node) {
	if(node == NULL) {
		logprintf(LOG_ERR, "internal error: table object not passed or already freed");
		return 0;
	}

	if(lua_gettop(L) != 0) {
		pluaL_error(L, "metatable pop requires 0 arguments, %d given", lua_gettop(L));
	}

	int max = 0, x = 0, maxidx = 0, i = 0;

	uv_mutex_lock(&node->lock);
	for(x=0;x<node->nrvar;x++) {
		uv_mutex_lock(&node->table[x].lock);
		if(node->table[x].key.type_ == LUA_TSTRING) {
			uv_mutex_unlock(&node->table[x].lock);
			uv_mutex_unlock(&node->lock);
			pluaL_error(L, "metatable push only works on numeric arrays");
		} else {
			if(node->table[x].key.number_ >= max) {
				max = (int)node->table[x].key.number_;
				maxidx = x;
			}
		}
		uv_mutex_unlock(&node->table[x].lock);
	}

	uv_mutex_lock(&node->table[maxidx].lock);
	switch(node->table[maxidx].val.type_) {
		case LUA_TBOOLEAN: {
			lua_pushboolean(L, node->table[maxidx].val.number_);
		} break;
		case LUA_TNUMBER: {
			lua_pushnumber(L, node->table[maxidx].val.number_);
		} break;
		case LUA_TSTRING: {
			lua_pushstring(L, node->table[maxidx].val.string_);
			FREE(node->table[maxidx].val.string_);
		} break;
		case LUA_TTABLE: {
			push_plua_metatable(L, (struct plua_metatable_t *)node->table[maxidx].val.void_);
			plua_gc_reg(L, node->table[maxidx].val.void_, plua_metatable_unref);
		} break;
		default: {
			lua_pushnil(L);
		} break;
	}
	uv_mutex_unlock(&node->table[maxidx].lock);

	for(i=maxidx;i<node->nrvar-1;i++) {
		uv_mutex_lock(&node->table[i].lock);
		uv_mutex_lock(&node->table[i+1].lock);
		switch(node->table[i+1].val.type_) {
			case LUA_TNUMBER: {
				node->table[i].val.number_ = node->table[i+1].val.number_;
				node->table[i].val.type_ = node->table[i+1].val.type_;
			} break;
			case LUA_TSTRING: {
				node->table[i].val.string_ = node->table[i+1].val.string_;
				node->table[i].val.type_ = node->table[i+1].val.type_;
			} break;
			case LUA_TTABLE: {
				node->table[i].val.void_ = node->table[i+1].val.void_;
				node->table[i].val.type_ = node->table[i+1].val.type_;
			} break;
		}
		switch(node->table[i+1].key.type_) {
			case LUA_TNUMBER: {
				node->table[i].key.number_ = (int)node->table[i+1].key.number_ - 1;
				node->table[i].key.type_ = node->table[i+1].key.type_;
			} break;
			case LUA_TSTRING: {
				node->table[i].key.string_ = node->table[i+1].key.string_;
				node->table[i].key.type_ = node->table[i+1].key.type_;
			}
		}
		uv_mutex_unlock(&node->table[i].lock);
		uv_mutex_unlock(&node->table[i+1].lock);
	}
	node->nrvar--;
	uv_mutex_unlock(&node->lock);

	return 1;
}

static int plua_metatable__pop(lua_State *L) {
	struct plua_metatable_t *node = (void *)lua_topointer(L, lua_upvalueindex(1));

	return plua_metatable_pop(L, node);
}

static int plua_metatable___pop(lua_State *L) {
	struct plua_interface_t *interface = (void *)lua_topointer(L, lua_upvalueindex(1));
	struct plua_metatable_t *node = interface->table;

	return plua_metatable_pop(L, node);
}

static int plua_metatable_unshift(lua_State *L, struct plua_metatable_t *node) {
	if(node == NULL) {
		logprintf(LOG_ERR, "internal error: table object not passed");
		return 0;
	}

	if(lua_gettop(L) != 0) {
		pluaL_error(L, "metatable unshift requires 0 arguments, %d given", lua_gettop(L));
	}

	int x = 0, i = 0, minidx = 0;

	uv_mutex_lock(&node->lock);
	for(x=0;x<node->nrvar;x++) {
		uv_mutex_lock(&node->table[x].lock);
		if(node->table[x].key.type_ == LUA_TSTRING) {
			uv_mutex_unlock(&node->table[x].lock);
			uv_mutex_unlock(&node->lock);
			pluaL_error(L, "metatable unshift only works on numeric arrays");
		} else {
			if(node->table[x].key.number_ == 1) {
				minidx = x;
			}
		}
		uv_mutex_unlock(&node->table[x].lock);
	}

	if(minidx >= 0) {
		uv_mutex_lock(&node->table[minidx].lock);
		switch(node->table[minidx].val.type_) {
			case LUA_TBOOLEAN: {
				lua_pushboolean(L, node->table[minidx].val.number_);
			} break;
			case LUA_TNUMBER: {
				lua_pushnumber(L, node->table[minidx].val.number_);
			} break;
			case LUA_TSTRING: {
				lua_pushstring(L, node->table[minidx].val.string_);
				FREE(node->table[minidx].val.string_);
			} break;
			case LUA_TTABLE: {
				push_plua_metatable(L, (struct plua_metatable_t *)node->table[minidx].val.void_);
				plua_gc_reg(L, node->table[minidx].val.void_, plua_metatable_unref);
			} break;
			default: {
				lua_pushnil(L);
			} break;
		}
		uv_mutex_unlock(&node->table[minidx].lock);

		for(i=minidx;i<node->nrvar-1;i++) {
			uv_mutex_lock(&node->table[i].lock);
			uv_mutex_lock(&node->table[i+1].lock);
			switch(node->table[i+1].val.type_) {
				case LUA_TNUMBER: {
					node->table[i].val.number_ = node->table[i+1].val.number_;
					node->table[i].val.type_ = node->table[i+1].val.type_;
				} break;
				case LUA_TSTRING: {
					node->table[i].val.string_ = node->table[i+1].val.string_;
					node->table[i].val.type_ = node->table[i+1].val.type_;
				} break;
				case LUA_TTABLE: {
					node->table[i].val.void_ = node->table[i+1].val.void_;
					node->table[i].val.type_ = node->table[i+1].val.type_;
				} break;
			}
			switch(node->table[i+1].key.type_) {
				case LUA_TNUMBER: {
					node->table[i].key.number_ = (int)node->table[i+1].key.number_ - 1;
					node->table[i].key.type_ = node->table[i+1].key.type_;
				} break;
				case LUA_TSTRING: {
					node->table[i].key.string_ = node->table[i+1].key.string_;
					node->table[i].key.type_ = node->table[i+1].key.type_;
				}
			}
			uv_mutex_unlock(&node->table[i].lock);
			uv_mutex_unlock(&node->table[i+1].lock);
		}
		node->nrvar--;
		uv_mutex_unlock(&node->lock);
		return 1;
	}

	lua_pushnil(L);

	uv_mutex_unlock(&node->lock);

	return 1;
}

static int plua_metatable__unshift(lua_State *L) {
	struct plua_metatable_t *node = (void *)lua_topointer(L, lua_upvalueindex(1));

	return plua_metatable_unshift(L, node);
}

static int plua_metatable___unshift(lua_State *L) {
	struct plua_interface_t *interface = (void *)lua_topointer(L, lua_upvalueindex(1));
	struct plua_metatable_t *node = interface->table;

	return plua_metatable_unshift(L, node);
}

static int plua_metatable_shift(lua_State *L, struct plua_metatable_t *node) {
	char buf[128] = { '\0' }, *p = buf;
	char *error = "string, number, table, boolean or nil expected, got %s";

	if(node == NULL) {
		logprintf(LOG_ERR, "internal error: table object not passed or already freed");
		return 0;
	}

	if(lua_gettop(L) != 1) {
		pluaL_error(L, "metatable shift requires 0 arguments, %d given", lua_gettop(L));
	}

	sprintf(p, error, lua_typename(L, lua_type(L, -1)));

	luaL_argcheck(L,
		((lua_type(L, -1) == LUA_TSTRING) || (lua_type(L, -1) == LUA_TNUMBER) ||
		 (lua_type(L, -1) == LUA_TTABLE || (lua_type(L, -1) == LUA_TBOOLEAN))),
		1, buf);

	int x = 0, i = 0;

	uv_mutex_lock(&node->lock);
	for(x=0;x<node->nrvar;x++) {
		uv_mutex_lock(&node->table[x].lock);
		if(node->table[x].key.type_ == LUA_TSTRING) {
			uv_mutex_unlock(&node->table[x].lock);
			uv_mutex_unlock(&node->lock);
			pluaL_error(L, "metatable shift only works on numeric arrays");
		}
		uv_mutex_unlock(&node->table[x].lock);
	}

	int idx = node->nrvar;
	if((node->table = REALLOC(node->table, sizeof(*node->table)*(idx+1))) == NULL) {
		OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
	}

	for(i=node->nrvar-1;i>=0;--i) {
		uv_mutex_lock(&node->table[i].lock);
		switch(node->table[i].key.type_) {
			case LUA_TNUMBER: {
				node->table[i].key.number_ = (int)node->table[i].key.number_ + 1;
			} break;
		}
		uv_mutex_unlock(&node->table[i].lock);
	}

	uv_mutex_init(&node->table[idx].lock);
	switch(lua_type(L, -1)) {
		case LUA_TBOOLEAN: {
			node->table[idx].val.number_ = lua_toboolean(L, -1);
			node->table[idx].val.type_ = LUA_TBOOLEAN;
		} break;
		case LUA_TNUMBER: {
			node->table[idx].val.number_ = lua_tonumber(L, -1);
			node->table[idx].val.type_ = LUA_TNUMBER;
		} break;
		case LUA_TSTRING: {
			if((node->table[idx].val.string_ = STRDUP((char *)lua_tostring(L, -1))) == NULL) {
				OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
			};
			node->table[idx].val.type_ = LUA_TSTRING;
		} break;
		case LUA_TTABLE: {
			int is_metatable = 0;

			node->table[idx].val.void_ = NULL;

			node->table[idx].val.type_ = LUA_TTABLE;
			if((is_metatable = lua_getmetatable(L, -1)) == 1) {
				lua_remove(L, -1);
				if(luaL_getmetafield(L, -1, "__call")) {
					if(plua_pcall(L, __FILE__, 1, 1) == 0) {
						if(lua_type(L, -1) == LUA_TLIGHTUSERDATA) {
							struct plua_metatable_t *table = lua_touserdata(L, -1);
							plua_metatable_clone(&table, (struct plua_metatable_t **)&node->table[idx].val.void_);
						} else {
							logprintf(LOG_ERR, "metatable metafield __call does not return userdata");
						}
					}
				} else {
					logprintf(LOG_ERR, "metatable does not the call metafield");
				}
			} else {
				plua_metatable_init((struct plua_metatable_t **)&node->table[idx].val.void_);
				lua_pushnil(L);
				while(lua_next(L, -2) != 0) {
					plua_metatable_parse_set(L, node->table[idx].val.void_);
					lua_pop(L, 1);
				}
			}
		} break;
	}

	node->table[idx].key.number_ = 1;
	node->table[idx].key.type_ = LUA_TNUMBER;
	node->nrvar++;

	uv_mutex_unlock(&node->lock);

	return 1;
}

static int plua_metatable__shift(lua_State *L) {
	struct plua_metatable_t *node = (void *)lua_topointer(L, lua_upvalueindex(1));

	return plua_metatable_shift(L, node);
}

static int plua_metatable___shift(lua_State *L) {
	struct plua_interface_t *interface = (void *)lua_topointer(L, lua_upvalueindex(1));
	struct plua_metatable_t *node = interface->table;

	return plua_metatable_shift(L, node);
}

void push__plua_metatable(lua_State *L, struct plua_interface_t *table) {
	uv_sem_post(table->table->ref);

	plua_gc_reg(L, table->table, plua_metatable_unref);

	lua_newtable(L);

	lua_pushstring(L, "len");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable___len, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "push");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable___push, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "pop");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable___pop, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "unshift");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable___unshift, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "shift");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable___shift, 1);
	lua_settable(L, -3);

	lua_newtable(L);

	lua_pushstring(L, "__index");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable___index, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "__newindex");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable___newindex, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "__gc");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable___gc, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "__pairs");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable___pairs, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "__ipairs");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable___ipairs, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "__next");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable___next, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "__call");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable___call, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "__metatable");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable___metatable, 1);
	lua_settable(L, -3);

	lua_setmetatable(L, -2);
}

void push_plua_metatable(lua_State *L, struct plua_metatable_t *table) {
	uv_sem_post(table->ref);

	plua_gc_reg(L, table, plua_metatable_unref);

	lua_newtable(L);

	lua_pushstring(L, "len");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable__len, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "push");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable__push, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "pop");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable__pop, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "unshift");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable__unshift, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "shift");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable__shift, 1);
	lua_settable(L, -3);

	lua_newtable(L);

	lua_pushstring(L, "__index");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable__index, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "__newindex");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable__newindex, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "__gc");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable__gc, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "__pairs");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable__pairs, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "__ipairs");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable__ipairs, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "__next");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable__next, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "__call");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable__call, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "__metatable");
	lua_pushlightuserdata(L, table);
	lua_pushcclosure(L, plua_metatable__metatable, 1);
	lua_settable(L, -3);

	lua_setmetatable(L, -2);
}

static int plua_metatable_metatable(lua_State *L, struct plua_metatable_t *node) {
	if(node == NULL) {
		logprintf(LOG_ERR, "internal error: table object not passed");
		return 0;
	}

	struct plua_metatable_t *tmp = NULL;

	plua_metatable_clone(&node, &tmp);

	plua_gc_reg(L, tmp, plua_metatable_unref);

	lua_pushlightuserdata(L, tmp);

	return 1;
}

static int plua_metatable__metatable(lua_State *L) {
	struct plua_metatable_t *node = (void *)lua_topointer(L, lua_upvalueindex(1));

	return plua_metatable_metatable(L, node);
}

static int plua_metatable___metatable(lua_State *L) {
	struct plua_interface_t *interface = (void *)lua_topointer(L, lua_upvalueindex(1));
	struct plua_metatable_t *node = interface->table;

	return plua_metatable_metatable(L, node);
}

void plua_metatable_free(struct plua_metatable_t *table) {
	int x = 0, y = 0;

	uv_mutex_lock(&table->lock);
	if(table->ref != NULL) {
		y = uv_sem_trywait(table->ref);
	}

	if((table->ref == NULL) || (y == UV__EAGAIN)) {
		for(x=0;x<table->nrvar;x++) {
			uv_mutex_lock(&table->table[x].lock);
			if(table->table[x].val.type_ == LUA_TSTRING) {
				FREE(table->table[x].val.string_);
			}
			if(table->table[x].key.type_ == LUA_TSTRING) {
				FREE(table->table[x].key.string_);
			}
			if(table->table[x].val.type_ == LUA_TTABLE) {
				plua_metatable_free(table->table[x].val.void_);
			}
			uv_mutex_unlock(&table->table[x].lock);
		}
		if(table->table != NULL) {
			FREE(table->table);
		}
		if(table->ref != NULL) {
			FREE(table->ref);
		}
		uv_mutex_unlock(&table->lock);
		uv_mutex_destroy(&table->lock);
		FREE(table);
	} else {
		uv_mutex_unlock(&table->lock);
	}
}

static int plua_metatable_call(lua_State *L, struct plua_metatable_t *node) {
	if(node == NULL) {
		logprintf(LOG_ERR, "internal error: table object not passed");
		return 0;
	}

	lua_pushlightuserdata(L, node);

	return 1;
}

static int plua_metatable__call(lua_State *L) {
	struct plua_metatable_t *node = (void *)lua_topointer(L, lua_upvalueindex(1));

	return plua_metatable_call(L, node);
}

static int plua_metatable___call(lua_State *L) {
	struct plua_interface_t *interface = (void *)lua_topointer(L, lua_upvalueindex(1));
	struct plua_metatable_t *node = interface->table;

	return plua_metatable_call(L, node);
}

void plua_metatable_clone(struct plua_metatable_t **src, struct plua_metatable_t **dst) {
	int i = 0;
	struct plua_metatable_t *a = *src;

	if((*dst) != NULL) {
		plua_metatable_free((*dst));
	}

	plua_metatable_init(&(*dst));

	uv_mutex_lock(&(*dst)->lock);
	uv_mutex_lock(&a->lock);

	if(((*dst)->table = MALLOC(sizeof(*a->table)*(a->nrvar))) == NULL) {
		OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
	}
	memset((*dst)->table, 0, sizeof(*a->table)*(a->nrvar));
	for(i=0;i<a->nrvar;i++) {
		uv_mutex_lock(&a->table[i].lock);
		uv_mutex_lock(&(*dst)->table[i].lock);
		(*dst)->table[i].key.type_ = a->table[i].key.type_;
		(*dst)->table[i].val.type_ = a->table[i].val.type_;

		if(a->table[i].key.type_ == LUA_TSTRING) {
			if(((*dst)->table[i].key.string_ = STRDUP(a->table[i].key.string_)) == NULL) {
				OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
			}
		}
		if(a->table[i].key.type_ == LUA_TNUMBER) {
			(*dst)->table[i].key.number_ = a->table[i].key.number_;
		}

		if(a->table[i].val.type_ == LUA_TSTRING) {
			if(((*dst)->table[i].val.string_ = STRDUP(a->table[i].val.string_)) == NULL) {
				OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
			}
		}
		if(a->table[i].val.type_ == LUA_TNUMBER || a->table[i].val.type_ == LUA_TBOOLEAN) {
			(*dst)->table[i].val.number_ = a->table[i].val.number_;
		}
		if(a->table[i].val.type_ == LUA_TTABLE) {
			plua_metatable_clone((struct plua_metatable_t **)&a->table[i].val.void_, (struct plua_metatable_t **)&(*dst)->table[i].val.void_);
		}
		uv_mutex_unlock(&a->table[i].lock);
		uv_mutex_unlock(&(*dst)->table[i].lock);
	}
	(*dst)->nrvar = a->nrvar;

	uv_mutex_unlock(&(*dst)->lock);
	uv_mutex_unlock(&a->lock);
}

static int plua_metatable_next(lua_State *L, struct plua_metatable_t *node) {
	struct lua_state_t *state = plua_get_current_state(L);

	if(node == NULL) {
		logprintf(LOG_ERR, "internal error: table object not passed");
		return 0;
	}

	uv_mutex_lock(&node->lock);

	int iter = node->iter[state->idx]++;

	if(node->nrvar > iter) {
		uv_mutex_lock(&node->table[iter].lock);
		switch(node->table[iter].key.type_) {
			case LUA_TNUMBER: {
				lua_pushnumber(L, node->table[iter].key.number_);
			} break;
			case LUA_TSTRING: {
				lua_pushstring(L, node->table[iter].key.string_);
			}
		}
		switch(node->table[iter].val.type_) {
			case LUA_TNUMBER: {
				lua_pushnumber(L, node->table[iter].val.number_);
			} break;
			case LUA_TBOOLEAN: {
				lua_pushboolean(L, node->table[iter].val.number_);
			} break;
			case LUA_TSTRING: {
				lua_pushstring(L, node->table[iter].val.string_);
			} break;
			case LUA_TTABLE: {
				push_plua_metatable(L, (struct plua_metatable_t *)node->table[iter].val.void_);
			} break;
		}

		uv_mutex_unlock(&node->table[iter].lock);
		uv_mutex_unlock(&node->lock);

		return 2;
	}

	lua_pushnil(L);

	uv_mutex_unlock(&node->lock);
  return 1;
}

static int plua_metatable__next(lua_State *L) {
	struct plua_metatable_t *node = (void *)lua_topointer(L, lua_upvalueindex(1));

	return plua_metatable_next(L, node);
}

static int plua_metatable___next(lua_State *L) {
	struct plua_interface_t *interface = (void *)lua_topointer(L, lua_upvalueindex(1));
	struct plua_metatable_t *node = interface->table;

	return plua_metatable_next(L, node);
}

static int plua_metatable_pairs(lua_State *L, struct plua_metatable_t *node) {
	struct lua_state_t *state = plua_get_current_state(L);

	if(node == NULL) {
		logprintf(LOG_ERR, "internal error: table object not passed");
		return 0;
	}

	uv_mutex_lock(&node->lock);

	node->iter[state->idx] = 0;
	lua_pushlightuserdata(L, node);
	lua_pushcclosure(L, plua_metatable__next, 1);
	lua_pushvalue(L, 1);
	lua_pushnil(L);

	uv_mutex_unlock(&node->lock);

  return 3;
}

static int plua_metatable__pairs(lua_State *L) {
	struct plua_metatable_t *node = (void *)lua_topointer(L, lua_upvalueindex(1));

	return plua_metatable_pairs(L, node);
}

static int plua_metatable___pairs(lua_State *L) {
	struct plua_interface_t *interface = (void *)lua_topointer(L, lua_upvalueindex(1));
	struct plua_metatable_t *node = interface->table;

	return plua_metatable_pairs(L, node);
}

static int plua_metatable_ipairs(lua_State *L, struct plua_metatable_t *node) {
	struct lua_state_t *state = plua_get_current_state(L);

	if(node == NULL) {
		logprintf(LOG_ERR, "internal error: table object not passed");
		return 0;
	}

	uv_mutex_lock(&node->lock);

	node->iter[state->idx] = 0;
	lua_pushlightuserdata(L, node);
	lua_pushcclosure(L, plua_metatable__next, 1);
	lua_pushvalue(L, 1);
	lua_pushinteger(L, 0);

	uv_mutex_unlock(&node->lock);

  return 3;
}

static int plua_metatable__ipairs(lua_State *L) {
	struct plua_metatable_t *node = (void *)lua_topointer(L, lua_upvalueindex(1));

	return plua_metatable_ipairs(L, node);
}

static int plua_metatable___ipairs(lua_State *L) {
	struct plua_interface_t *interface = (void *)lua_topointer(L, lua_upvalueindex(1));
	struct plua_metatable_t *node = interface->table;

	return plua_metatable_ipairs(L, node);
}

static int plua_metatable_index(lua_State *L, struct plua_metatable_t *node) {
	char buf[128] = { '\0' }, *p = buf;
	char *error = "string or number expected, got %s";
	int x = 0, match = 0;

	if(node == NULL) {
		logprintf(LOG_ERR, "internal error: table object not passed or already freed");
		return 0;
	}

	sprintf(p, error, lua_typename(L, lua_type(L, -1)));

	luaL_argcheck(L,
		((lua_type(L, -1) == LUA_TSTRING) || (lua_type(L, -1) == LUA_TNUMBER)),
		1, buf);

	uv_mutex_lock(&node->lock);

	for(x=0;x<node->nrvar;x++) {
		match = 0;
		uv_mutex_lock(&node->table[x].lock);
		switch(lua_type(L, -1)) {
			case LUA_TNUMBER: {
				if(node->table[x].key.type_ == LUA_TNUMBER &&
					node->table[x].key.number_ == (int)lua_tonumber(L, -1)) {
					match = 1;
				}
			} break;
			case LUA_TSTRING: {
				if(node->table[x].key.type_ == LUA_TSTRING &&
				 strcmp(node->table[x].key.string_, lua_tostring(L, -1)) == 0) {
					match = 1;
				}
			}
		}
		if(match == 1) {
			switch(node->table[x].val.type_) {
				case LUA_TBOOLEAN: {
					lua_pushboolean(L, node->table[x].val.number_);
				} break;
				case LUA_TNUMBER: {
					lua_pushnumber(L, node->table[x].val.number_);
				} break;
				case LUA_TSTRING: {
					lua_pushstring(L, node->table[x].val.string_);
				} break;
				case LUA_TTABLE: {
					push_plua_metatable(L, (struct plua_metatable_t *)node->table[x].val.void_);
				} break;
				default: {
					lua_pushnil(L);
				} break;
			}
			uv_mutex_unlock(&node->table[x].lock);
			uv_mutex_unlock(&node->lock);

			return 1;
		}
		uv_mutex_unlock(&node->table[x].lock);
	}

	lua_pushnil(L);

	uv_mutex_unlock(&node->lock);

	return 0;
}

static int plua_metatable__index(lua_State *L) {
	struct plua_metatable_t *node = (void *)lua_topointer(L, lua_upvalueindex(1));

	return plua_metatable_index(L, node);
}

static int plua_metatable___index(lua_State *L) {
	struct plua_interface_t *interface = (void *)lua_topointer(L, lua_upvalueindex(1));
	struct plua_metatable_t *node = interface->table;

	return plua_metatable_index(L, node);
}

void plua_metatable_parse_set(lua_State *L, void *data) {
	struct plua_metatable_t *node = data;

	char buf[128] = { '\0' }, *p = buf;
	char *error1 = "string, number, table, boolean or nil expected, got %s";
	char *error2 = "string or number expected, got %s";
	int match = 0, x = 0;

	if(data == NULL) {
		logprintf(LOG_ERR, "internal error: table object not passed or already freed");
		return;
	}

	sprintf(p, error1, lua_typename(L, lua_type(L, -1)));

	luaL_argcheck(L,
		((lua_type(L, -1) == LUA_TSTRING) || (lua_type(L, -1) == LUA_TNUMBER) ||
		(lua_type(L, -1) == LUA_TNIL) || (lua_type(L, -1) == LUA_TBOOLEAN) || (lua_type(L, -1) == LUA_TTABLE)),
		1, buf);

	sprintf(p, error2, lua_typename(L, lua_type(L, -2)));

	luaL_argcheck(L,
		((lua_type(L, -2) == LUA_TSTRING) || (lua_type(L, -2) == LUA_TNUMBER)),
		1, buf);

	uv_mutex_lock(&node->lock);

	for(x=0;x<node->nrvar;x++) {
		uv_mutex_lock(&node->table[x].lock);
		switch(lua_type(L, -2)) {
			case LUA_TNUMBER: {
				if(node->table[x].key.type_ == LUA_TNUMBER &&
					node->table[x].key.number_ == (int)lua_tonumber(L, -2)) {
					match = 1;
				}
			} break;
			case LUA_TSTRING: {
				if(node->table[x].key.type_ == LUA_TSTRING &&
					strcmp(node->table[x].key.string_, lua_tostring(L, -2)) == 0) {
					match = 1;
				}
			} break;
		}
		if(match == 1) {
			switch(lua_type(L, -1)) {
				case LUA_TBOOLEAN: {
					if(node->table[x].val.type_ == LUA_TSTRING) {
						FREE(node->table[x].val.string_);
					}
					if(node->table[x].val.type_ == LUA_TTABLE) {
						plua_metatable_free(node->table[x].val.void_);
					}
					node->table[x].val.number_ = lua_toboolean(L, -1);
					node->table[x].val.type_ = LUA_TBOOLEAN;

					uv_mutex_unlock(&node->table[x].lock);
				} break;
				case LUA_TNUMBER: {
					if(node->table[x].val.type_ == LUA_TSTRING) {
						FREE(node->table[x].val.string_);
					}
					if(node->table[x].val.type_ == LUA_TTABLE) {
						plua_metatable_free(node->table[x].val.void_);
					}
					node->table[x].val.number_ = lua_tonumber(L, -1);
					node->table[x].val.type_ = LUA_TNUMBER;

					uv_mutex_unlock(&node->table[x].lock);
				} break;
				case LUA_TSTRING: {
					if(node->table[x].val.type_ == LUA_TSTRING) {
						FREE(node->table[x].val.string_);
					}
					if(node->table[x].val.type_ == LUA_TTABLE) {
						plua_metatable_free(node->table[x].val.void_);
					}
					if((node->table[x].val.string_ = STRDUP((char *)lua_tostring(L, -1))) == NULL) {
						OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
					}
					node->table[x].val.type_ = LUA_TSTRING;

					uv_mutex_unlock(&node->table[x].lock);
				} break;
				case LUA_TTABLE: {
					int is_metatable = 0;
					if(node->table[x].val.type_ == LUA_TSTRING) {
						FREE(node->table[x].val.string_);
					}
					if(node->table[x].val.type_ == LUA_TTABLE) {
						plua_metatable_free(node->table[x].val.void_);
					}
					node->table[x].val.void_ = NULL;

					node->table[x].val.type_ = LUA_TTABLE;
					if((is_metatable = lua_getmetatable(L, -1)) == 1) {
						lua_remove(L, -1);
						if(luaL_getmetafield(L, -1, "__call")) {
							if(plua_pcall(L, __FILE__, 1, 1) == 0) {
								if(lua_type(L, -1) == LUA_TLIGHTUSERDATA) {
									struct plua_metatable_t *table = lua_touserdata(L, -1);
									plua_metatable_clone(&table, (struct plua_metatable_t **)&node->table[x].val.void_);
								} else {
									logprintf(LOG_ERR, "metatable metafield __call does not return userdata");
								}
							}
						} else {
							logprintf(LOG_ERR, "metatable does not have the call metafield");
						}
					} else {
						plua_metatable_init((struct plua_metatable_t **)&node->table[x].val.void_);
						lua_pushnil(L);
						while(lua_next(L, -2) != 0) {
							plua_metatable_parse_set(L, node->table[x].val.void_);
							lua_pop(L, 1);
						}
					}

					uv_mutex_unlock(&node->table[x].lock);
				} break;
				/*
				 * Remove key
				 */
				case LUA_TNIL: {
					int i = 0;
					match = 0;

					if(node->table[x].key.type_ == LUA_TSTRING) {
						FREE(node->table[x].key.string_);
					}
					if(node->table[x].val.type_ == LUA_TSTRING) {
						FREE(node->table[x].val.string_);
					}
					if(node->table[x].val.type_ == LUA_TTABLE) {
						plua_metatable_free(node->table[x].val.void_);
						if(node->nrvar == 0) {
							FREE(node->table);
						}
					} else {
						uv_mutex_unlock(&node->table[x].lock);

						for(i=x;i<node->nrvar-1;i++) {
							uv_mutex_lock(&node->table[i].lock);
							uv_mutex_lock(&node->table[i+1].lock);
							switch(node->table[i+1].val.type_) {
								case LUA_TNUMBER: {
									node->table[i].val.number_ = node->table[i+1].val.number_;
									node->table[i].val.type_ = node->table[i+1].val.type_;
								} break;
								case LUA_TSTRING: {
									node->table[i].val.string_ = node->table[i+1].val.string_;
									node->table[i].val.type_ = node->table[i+1].val.type_;
								} break;
								case LUA_TTABLE: {
									node->table[i].val.void_ = node->table[i+1].val.void_;
									node->table[i].val.type_ = node->table[i+1].val.type_;
								} break;
							}
							switch(node->table[i+1].key.type_) {
								case LUA_TNUMBER: {
									node->table[i].key.number_ = node->table[i+1].key.number_;
									node->table[i].key.type_ = node->table[i+1].key.type_;
								} break;
								case LUA_TSTRING: {
									node->table[i].key.string_ = node->table[i+1].key.string_;
									node->table[i].key.type_ = node->table[i+1].key.type_;
								}
							}
							uv_mutex_unlock(&node->table[i].lock);
							uv_mutex_unlock(&node->table[i+1].lock);
						}
					}
					node->nrvar--;
				} break;
			}

			break;
		}
		uv_mutex_unlock(&node->table[x].lock);
	}

	if(node != NULL) {
		if(match == 0 && lua_type(L, -1) != LUA_TNIL) {
			int idx = node->nrvar;
			if((node->table = REALLOC(node->table, sizeof(*node->table)*(idx+1))) == NULL) {
				OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
			}
			uv_mutex_init(&node->table[idx].lock);
			switch(lua_type(L, -1)) {
				case LUA_TBOOLEAN: {
					node->table[idx].val.number_ = lua_toboolean(L, -1);
					node->table[idx].val.type_ = LUA_TBOOLEAN;
				} break;
				case LUA_TNUMBER: {
					node->table[idx].val.number_ = lua_tonumber(L, -1);
					node->table[idx].val.type_ = LUA_TNUMBER;
				} break;
				case LUA_TSTRING: {
					if((node->table[idx].val.string_ = STRDUP((char *)lua_tostring(L, -1))) == NULL) {
						OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
					};
					node->table[idx].val.type_ = LUA_TSTRING;
				} break;
				case LUA_TTABLE: {
					int is_metatable = 0;

					node->table[idx].val.void_ = NULL;

					node->table[idx].val.type_ = LUA_TTABLE;
					if((is_metatable = lua_getmetatable(L, -1)) == 1) {
						lua_remove(L, -1);
						if(luaL_getmetafield(L, -1, "__call")) {
							if(plua_pcall(L, __FILE__, 1, 1) == 0) {
								if(lua_type(L, -1) == LUA_TLIGHTUSERDATA) {
									struct plua_metatable_t *table = lua_touserdata(L, -1);
									plua_metatable_clone(&table, (struct plua_metatable_t **)&node->table[x].val.void_);
								} else {
									logprintf(LOG_ERR, "metatable metafield __call does not return userdata");
								}
							}
						} else {
							logprintf(LOG_ERR, "metatable does not the call metafield");
						}
					} else {
						plua_metatable_init((struct plua_metatable_t **)&node->table[idx].val.void_);
						lua_pushnil(L);
						while(lua_next(L, -2) != 0) {
							plua_metatable_parse_set(L, node->table[x].val.void_);
							lua_pop(L, 1);
						}
					}
				} break;
			}
			switch(lua_type(L, -2)) {
				case LUA_TNUMBER: {
					node->table[idx].key.number_ = lua_tonumber(L, -2);
					node->table[idx].key.type_ = LUA_TNUMBER;
				} break;
				case LUA_TSTRING: {
					if((node->table[idx].key.string_ = STRDUP((char *)lua_tostring(L, -2))) == NULL) {
						OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
					};
					node->table[idx].key.type_ = LUA_TSTRING;
				} break;
			}
			node->nrvar++;
		}
	}

	uv_mutex_unlock(&node->lock);
}

static int plua_metatable_newindex(lua_State *L, struct plua_metatable_t *node) {
	if(node == NULL) {
		logprintf(LOG_ERR, "internal error: table object not passed");
		return 0;
	}

	plua_metatable_parse_set(L, node);

	return 1;
}

static int plua_metatable__newindex(lua_State *L) {
	struct plua_metatable_t *node = (void *)lua_topointer(L, lua_upvalueindex(1));

	return plua_metatable_newindex(L, node);
}

static int plua_metatable___newindex(lua_State *L) {
	struct plua_interface_t *interface = (void *)lua_topointer(L, lua_upvalueindex(1));
	struct plua_metatable_t *node = interface->table;

	return plua_metatable_newindex(L, node);
}

static int plua_metatable__gc(lua_State *L) {
	logprintf(LOG_ERR, "%s", __FUNCTION__);
	return 0;
}

static int plua_metatable___gc(lua_State *L) {
	logprintf(LOG_ERR, "%s", __FUNCTION__);
	return 0;
}

struct lua_state_t *plua_get_free_state(void) {
	int i = 0;
	int error = 0;

	while(1) {
		for(i=0;i<NRLUASTATES;i++) {
			if(uv_mutex_trylock(&lua_state[i].lock) == 0) {
				return &lua_state[i];
			}
		}
		if(error == 0) {
			error = 1;
			logprintf(LOG_DEBUG, "waiting free lua state to become available");
		}
		uv_sem_wait(&sem_used_states);
	}
	return NULL;
}

struct lua_state_t *plua_get_current_state(lua_State *L) {
	int i = 0;
	for(i=0;i<NRLUASTATES;i++) {
		if(lua_state[i].L == L) {
			return &lua_state[i];
		}
	}
	return NULL;
}

void _plua_clear_state(struct lua_state_t *state, char *file, int line) {
	int i = 0;
	uv_mutex_lock(&state->gc.lock);
	for(i=0;i<state->gc.nr;i++) {
		if(state->gc.list[i]->free == 0) {
			state->gc.list[i]->callback(state->gc.list[i]->ptr);
		}
		FREE(state->gc.list[i]);
	}
	if(state->gc.size > 0) {
		FREE(state->gc.list);
	}
	state->gc.nr = 0;
	state->gc.size = 0;

	state->oldmod = state->module;
	if(state->oldmod != NULL) {
		state->oldmod->btfile = file;
		state->oldmod->btline = line;
	}

	uv_mutex_unlock(&state->gc.lock);

	if(state->L != NULL) {
		assert(plua_check_stack(state->L, 0) == 0);
	}

	uv_mutex_unlock(&state->lock);
	uv_sem_post(&sem_used_states);
}

static int plua_get_table_string_by_key(struct lua_State *L, const char *key, const char **ret) {
	/*
	 * Push the key we want to retrieve on the stack
	 *
	 * stack now contains: -1 => key -2 => table
	 */
	lua_pushstring(L, key);

	if(lua_type(L, -2) != LUA_TTABLE) {
		/*
		 * Remove the key from the stack again
		 *
		 * stack now contains: -1 => table
		 */
		lua_pop(L, 1);
		return 0;
	}

	/*
	 * Replace the key at -1 with it value in table -2
	 *
	 * stack now contains: -1 => value -2 => table
	 */
	lua_gettable(L, -2);

	/*
	 * Check if the first element is a number
	 */
	if(lua_type(L, -1) != LUA_TSTRING) {
		/*
		 * Remove the value from the stack again
		 *
		 * stack now contains: -1 => table
		 */
		lua_pop(L, 1);
		return 0;
	}

	*ret = lua_tostring(L, -1);

	/*
	 * stack now contains: -1 => table
	 */
	lua_pop(L, 1);
	return 1;
}

// static int plua_get_table_double_by_key(struct lua_State *L, const char *key, double *ret) {
	// /*
	 // * Push the key we want to retrieve on the stack
	 // *
	 // * stack now contains: -1 => key -2 => table
	 // */
	// lua_pushstring(L, key);

	// /*
	 // * Replace the key at -1 with it value in table -2
	 // *
	 // * stack now contains: -1 => value -2 => table
	 // */
	// if(lua_istable(L, -2) == 0) {
		// /*
		 // * Remove the key from the stack again
		 // *
		 // * stack now contains: -1 => table
		 // */
		// lua_pop(L, 1);
		// return 0;
	// }
	// /*
	 // * Replace the key at -1 with it value in table -2
	 // *
	 // * stack now contains: -1 => value -2 => table
	 // */
	// lua_gettable(L, -2);

	// /*
	 // * Check if the first element is a number
	 // */
	// if(lua_isnumber(L, -1) == 0) {
		// /*
		 // * Remove the value from the stack again
		 // *
		 // * stack now contains: -1 => table
		 // */
		// lua_pop(L, 1);
		// return 0;
	// }

	// *ret = lua_tonumber(L, -1);

	// /*
	 // * stack now contains: -1 => table
	 // */
	// lua_pop(L, 1);

	// return 1;
// }

static int plua_table_has_keys(lua_State *L, char **keys, int number) {
	if(lua_type(L, -1) != LUA_TTABLE) {
		return 0;
	}

	int i = 0;
	/*
	 * Push another reference to the table on top of the stack (so we know
	 * where it is), and this function can work for negative, positive and
	 * pseudo indices.
	 *
	 * stack now contains: -1 => table -2 => table
	 */
	lua_pushvalue(L, -1);

	/*
	 * stack now contains: -1 => nil -2 => table -3 => table
	 */
	lua_pushnil(L);

	int match = 0, nrkeys = 0;
	while(lua_next(L, -2)) {
		nrkeys++;

		/*
		 * stack now contains: -1 => value -2 => key -3 => table
		 *
		 * copy the key so that lua_tostring does not modify the original
		 *
		 * stack now contains: -1 => key -2 => value; -3 => key -4 => table -5 => table
		 */
		lua_pushvalue(L, -2);

		const char *k = lua_tostring(L, -1); // key

		for(i=0;i<number;i++) {
			if(strcmp(keys[i], k) == 0) {
				match++;
				break;
			}
		}

		/*
		 * pop value + copy of key, leaving original key
		 *
		 * stack now contains: -1 => key -2 => table -3 => table
		 */
		lua_pop(L, 2);
	}
	/*
	 * After the last lua_next call stack now contains:
	 * -1 => table -2 => table
	 */
	if(match != number || nrkeys != number) {
		lua_pop(L, 1);
		return 0;
	}

	/*
	 * Remove duplicated table from stack
	 *
	 * stack now contains -1 => table
	 */
	lua_pop(L, 1);

	return 1;
}

static int plua_module_init(struct lua_State *L, char *file, struct plua_module_t *mod) {
	/*
	 * Function info is at top of stack
	 *
	 * stack now contains -1 => function
	 */
#if LUA_VERSION_NUM <= 502
	lua_getfield(L, -1, "info");
	if(lua_type(L, -1) != LUA_TFUNCTION) {
#else
	if(lua_getfield(L, -1, "info") == 0) {
#endif
		logprintf(LOG_ERR, "%s: info function missing", file);
		assert(plua_check_stack(L, 0) == 0);
		return -1;
	}

	char *type = NULL;
	switch(mod->type) {
#ifdef PILIGHT_UNITTEST
		case UNITTEST: {
			type = STRDUP("unittest");
		} break;
#endif
		case FUNCTION: {
			type = STRDUP("event function");
		} break;
		case OPERATOR: {
			type = STRDUP("event operator");
		} break;
		case ACTION: {
			type = STRDUP("event action");
		} break;
		case PROTOCOL: {
			type = STRDUP("protocol");
		} break;
		case STORAGE: {
			type = STRDUP("storage");
		} break;
		case HARDWARE: {
			type = STRDUP("hardware");
		} break;
	}
	if(type == NULL) {
		OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
	}

	/*
	 * Returned table (first argument) is at top of stack
	 *
	 * stack now contains -1 => function
	 */
	assert(plua_check_stack(L, 2, PLUA_TTABLE, PLUA_TFUNCTION) == 0);
	if(plua_pcall(L, __FILE__, 0, 1) == -1) {
		assert(plua_check_stack(L, 0) == 0);
		return -1;
	}

	lua_remove(L, 1);

	assert(plua_check_stack(L, 1, PLUA_TTABLE) == 0);
	if(lua_type(L, -1) != LUA_TTABLE) {
		logprintf(LOG_ERR, "%s: the info function returned %s, table expected", file, lua_typename(L, lua_type(L, -1)));
		assert(plua_check_stack(L, 0) == 0);
		FREE(type);
		return -1;
	}

	char *keys[12] = {"name", "version", "reqversion", "reqcommit"};
	if(plua_table_has_keys(L, keys, 4) == 0) {
		logprintf(LOG_ERR, "%s: the info table has invalid keys", file);
		assert(plua_check_stack(L, 0) == 0);
		FREE(type);
		return -1;
	}

	const char *name = NULL, *version = NULL, *reqversion = NULL, *reqcommit = NULL;
	if(plua_get_table_string_by_key(L, "name", &name) == 0) {
		logprintf(LOG_ERR, "%s: the info table 'name' key is missing or invalid", file);
		assert(plua_check_stack(L, 0) == 0);
		FREE(type);
		return -1;
	}

	if(plua_get_table_string_by_key(L, "version", &version) == 0) {
		logprintf(LOG_ERR, "%s: the info table 'version' key is missing or invalid", file);
		assert(plua_check_stack(L, 0) == 0);
		FREE(type);
		return -1;
	}

	if(plua_get_table_string_by_key(L, "reqversion", &reqversion) == 0) {
		logprintf(LOG_ERR, "%s: the info table 'reqversion' key is missing or invalid", file);
		assert(plua_check_stack(L, 0) == 0);
		FREE(type);
		return -1;
	}
	if(plua_get_table_string_by_key(L, "reqcommit", &reqcommit) == 0) {
		logprintf(LOG_ERR, "%s: the info table 'reqcommit' key is missing or invalid", file);
		assert(plua_check_stack(L, 0) == 0);
		FREE(type);
		return -1;
	}

	strcpy(mod->name, name);
	strcpy(mod->version, version);
	strcpy(mod->reqversion, reqversion);
	strcpy(mod->reqcommit, reqcommit);

	char pilight_version[strlen(PILIGHT_VERSION)+1];
	char pilight_commit[3], *v = (char *)reqversion, *r = (char *)reqcommit;
	int valid = 1, check = 1;
	strcpy(pilight_version, PILIGHT_VERSION);

	if((check = vercmp(v, pilight_version)) > 0) {
		valid = 0;
	}

	if(check == 0 && strlen(mod->reqcommit) > 0) {
		sscanf(HASH, "v%*[0-9].%*[0-9]-%[0-9]-%*[0-9a-zA-Z\n\r]", pilight_commit);

		if(strlen(pilight_commit) > 0 && (vercmp(r, pilight_commit)) > 0) {
			valid = 0;
		}
	}
	if(valid == 1) {
		logprintf(LOG_DEBUG, "loaded %s %s v%s", type, file, version);
	} else {
		if(strlen(mod->reqcommit) > 0) {
			logprintf(LOG_ERR, "%s %s requires at least pilight v%s (commit %s)", type, file, mod->reqversion, mod->reqcommit);
		} else {
			logprintf(LOG_ERR, "%s %s requires at least pilight v%s", type, file, mod->reqversion);
		}
		/*
		 * Pop function from stack
		 *
		 * The stack now contains: nothing
		 */
		lua_pop(L, 1);
		assert(plua_check_stack(L, 0) == 0);
		FREE(type);
		return -1;
	}

	/*
	 * Pop function from stack
	 *
	 * The stack now contains: nothing
	 */
	lua_remove(L, 1);
	assert(plua_check_stack(L, 0) == 0);
	FREE(type);
	return 0;
}

static int plua_writer(lua_State *L, const void* p, size_t sz, void* ud) {
	struct plua_module_t *module = ud;
	if((module->bytecode = REALLOC(module->bytecode, module->size+sz)) == NULL) {
		OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
	}
	memcpy(&module->bytecode[module->size], p, sz);
	module->size += sz;
	return 0;
}

void plua_module_load(char *file, int type) {
	struct plua_module_t *module = MALLOC(sizeof(struct plua_module_t));
	lua_State *L = lua_state[0].L;
	char name[512] = { '\0' }, *p = name;
	int i = 0;
	if(module == NULL) {
		OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
	}
	memset(module, 0, sizeof(struct plua_module_t));

	if(luaL_loadfile(L, file) != 0) {
		logprintf(LOG_ERR, "cannot load lua file: %s", file);
		lua_remove(L, -1);
		assert(plua_check_stack(L, 0) == 0);
		FREE(module);
		return;
	}
	strcpy(module->file, file);
	if(lua_dump(L, plua_writer, module) != 0) {
		logprintf(LOG_ERR, "cannot dump lua file: %s", file);
		lua_remove(L, -1);
		assert(plua_check_stack(L, 0) == 0);
		FREE(module->bytecode);
		FREE(module);
		return;
	}

	assert(plua_check_stack(L, 1, PLUA_TFUNCTION) == 0);
	if(plua_pcall(L, file, 0, LUA_MULTRET) == -1) {
		assert(plua_check_stack(L, 0) == 0);
		return;
	}

	assert(plua_check_stack(L, 1, PLUA_TTABLE) == 0);
	if(lua_type(L, -1) != LUA_TTABLE) {
		logprintf(LOG_ERR, "%s: does not return a table", file);
		lua_remove(L, -1);
		FREE(module->bytecode);
		FREE(module);
		return;
	}

	module->type = type;
	strcpy(module->file, file);
	if(plua_module_init(L, file, module) != -1) {
		memset(p, '\0', sizeof(name));
		switch(module->type) {
#ifdef PILIGHT_UNITTEST
			case UNITTEST:
				sprintf(p, "unittest.%s", module->name);
			break;
#endif
			case OPERATOR:
				sprintf(p, "operator.%s", module->name);
			break;
			case FUNCTION:
				sprintf(p, "function.%s", module->name);
			break;
			case ACTION:
				sprintf(p, "action.%s", module->name);
			break;
			case PROTOCOL:
				sprintf(p, "protocol.%s", module->name);
			break;
			case STORAGE:
				sprintf(p, "storage.%s", module->name);
			break;
			case HARDWARE:
				sprintf(p, "hardware.%s", module->name);
			break;
		}
		module->next = modules;
		modules = module;
	} else {
		FREE(module->bytecode);
		FREE(module);
		assert(plua_check_stack(L, 0) == 0);
		return;
	}
	luaL_loadbuffer(L, module->bytecode, module->size, module->name);
	assert(plua_check_stack(L, 1, PLUA_TFUNCTION) == 0);
	if(plua_pcall(L, module->file, 0, LUA_MULTRET) == -1) {
		assert(plua_check_stack(L, 0) == 0);
		return;
	}
	assert(lua_type(L, -1) == LUA_TTABLE);
	lua_setglobal(L, name);

	for(i=1;i<NRLUASTATES;i++) {
		lua_State *L = lua_state[i].L;
		luaL_loadbuffer(L, module->bytecode, module->size, module->name);
		assert(plua_check_stack(L, 1, PLUA_TFUNCTION) == 0);
		if(plua_pcall(L, module->file, 0, LUA_MULTRET) == -1) {
			assert(plua_check_stack(L, 0) == 0);
			return;
		}
		assert(plua_check_stack(L, 1, PLUA_TTABLE) == 0);
		lua_setglobal(L, name);
	}

	lua_getglobal(L, name);
	if(lua_type(L, -1) == LUA_TNIL) {
		lua_pop(L, -1);
		assert(plua_check_stack(L, 0) == 0);
		return;
	}
	if(lua_type(L, -1) != LUA_TTABLE) {
		lua_pop(L, -1);
		assert(plua_check_stack(L, 0) == 0);
		return;
	}

	assert(plua_check_stack(L, 1, PLUA_TTABLE) == 0);
#ifdef PILIGHT_UNITTEST
	if(module->type != UNITTEST) {
		if(lua_type(L, -1) == LUA_TTABLE) {
			lua_pushvalue(L, -1);
			lua_pushnil(L);

			/*
			 * Possible location of:
			 * 'invalid key to 'next'
			 */
			while(lua_next(L, -2)) {
				lua_pushvalue(L, -2);

				if(info.module != NULL) {
					FREE(info.module);
				}
				if(info.function != NULL) {
					FREE(info.function);
				}
				if((info.module = STRDUP((char *)name)) == NULL) {
					OUT_OF_MEMORY
				}
				if((info.function = STRDUP((char *)lua_tostring(L, -1))) == NULL) {
					OUT_OF_MEMORY
				}
				lua_insert(L, -2);

				if(lua_type(L, -1) == LUA_TFUNCTION) {
					plua_pcall(L, module->file, 0, LUA_MULTRET);
				}

				while(lua_gettop(L) > 3) {
					lua_remove(L, -1);
				}
			}
			lua_pop(L, 1);
		}
		lua_pop(L, -1);
	}

	if(info.module != NULL) {
		FREE(info.module);
	}
	if(info.function != NULL) {
		FREE(info.function);
	}
#endif
	lua_pop(L, -1);

	for(i=0;i<NRLUASTATES;i++) {
		assert(plua_check_stack(lua_state[i].L, 0) == 0);
	}
}


struct plua_module_t *plua_get_modules(void) {
	return modules;
}

// static void *plua_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
	// if(nsize == 0) {
		// if(ptr != NULL) {
			// FREE(ptr);
		// }
		// return 0;
	// } else {
		// return REALLOC(ptr, nsize);
	// }
// }

#ifdef PILIGHT_UNITTEST
void plua_pause_coverage(int status) {
	covpause = status;
}

static void hook(lua_State *L, lua_Debug *ar) {
	if(covfile == NULL) {
		return;
	}

	lua_getinfo(L, "nSfLl", ar);

	if(covpause == 1) {
		return;
	}

	int i = 0, match = -1;
	for(i=0;i<nrcoverage;i++) {
		if(strcmp(coverage[i]->file, ar->source) == 0) {
			match = i;
			break;
		}
	}

	if(match == -1) {
		if((coverage = realloc(coverage, sizeof(struct coverage_t *)*(nrcoverage+1))) == NULL) {
			OUT_OF_MEMORY
		}
		if((coverage[nrcoverage] = malloc(sizeof(struct coverage_t))) == NULL) {
			OUT_OF_MEMORY
		}
		memset(coverage[nrcoverage], 0, sizeof(struct coverage_t));

		if((coverage[nrcoverage]->file = strdup((char *)ar->source)) == NULL) {
			OUT_OF_MEMORY
		}

		match = nrcoverage;

		nrcoverage++;
	}

	while(ar->currentline >= coverage[match]->nrlstat || ar->linedefined >= coverage[match]->nrlstat) {
		if((coverage[match]->lstat = realloc(coverage[match]->lstat, sizeof(int)*(coverage[match]->nrlstat+512))) == NULL) {
			OUT_OF_MEMORY
		}
		for(i=coverage[match]->nrlstat;i<coverage[match]->nrlstat+512;i++) {
			coverage[match]->lstat[i] = -1;
		}
		coverage[match]->nrlstat += 512;
	}

	while(ar->currentline >= coverage[match]->nrfstat || ar->linedefined >= coverage[match]->nrfstat) {
		if((coverage[match]->fstat = realloc(coverage[match]->fstat, sizeof(int)*(coverage[match]->nrfstat+512))) == NULL) {
			OUT_OF_MEMORY
		}
		for(i=coverage[match]->nrfstat;i<coverage[match]->nrfstat+512;i++) {
			coverage[match]->fstat[i] = -1;
		}
		coverage[match]->nrfstat += 512;
	}

	while(ar->currentline >= coverage[match]->nrfunctions || ar->linedefined >= coverage[match]->nrfunctions) {
		if((coverage[match]->functions = realloc(coverage[match]->functions, sizeof(char *)*(coverage[match]->nrfunctions+12))) == NULL) {
			OUT_OF_MEMORY
		}
		for(i=coverage[match]->nrfunctions;i<coverage[match]->nrfunctions+12;i++) {
			coverage[match]->functions[i] = NULL;
		}
		coverage[match]->nrfunctions += 12;
	}

	if(coverage[match]->lstat[ar->currentline] == -1) {
		coverage[match]->lstat[ar->currentline] = 0;
	}

	if(coverage[match]->lstat[ar->linedefined] == -1) {
		coverage[match]->lstat[ar->linedefined] = 0;
	}

	coverage[match]->lstat[ar->currentline]++;

	if(info.module != NULL && coverage[match]->module == NULL) {
		if((coverage[match]->module = strdup(info.module)) == NULL) {
			OUT_OF_MEMORY
		}
	}
	if(info.function != NULL && coverage[match]->functions[ar->linedefined] == NULL) {
		if((coverage[match]->functions[ar->linedefined] = strdup(info.function)) == NULL) {
			OUT_OF_MEMORY
		}

		if(coverage[match]->fstat[ar->linedefined] == -1) {
			coverage[match]->fstat[ar->linedefined] = 0;
		}

		coverage[match]->fstat[ar->linedefined]++;
	}

	if(lua_type(L, -1) == LUA_TTABLE) {
		lua_pushvalue(L, -1);
		lua_pushnil(L);

		while(lua_next(L, -2)) {
			lua_pushvalue(L, -2);

			int line = lua_tonumber(L, -1);

			while(line >= coverage[match]->nrlstat) {
				if((coverage[match]->lstat = realloc(coverage[match]->lstat, sizeof(int)*(coverage[match]->nrlstat+512))) == NULL) {
					OUT_OF_MEMORY
				}
				for(i=coverage[match]->nrlstat;i<coverage[match]->nrlstat+512;i++) {
					coverage[match]->lstat[i] = -1;
				}
				coverage[match]->nrlstat += 512;
			}

			if(coverage[match]->lstat[line] == -1) {
				coverage[match]->lstat[line] = 0;
			}

			lua_pop(L, 2);
		}

		lua_pop(L, 1);
	}
}

void plua_coverage_output(const char *file) {
	covfile = file;
}
#endif

static int plua_atpanic(lua_State *L) {
	struct lua_state_t *state = plua_get_current_state(L);

	if(state != NULL) {
		if(state->file != NULL && state->line != -1) {
			logprintf(LOG_ERR, "(%s #%d) Lua panic (#%d): %s", state->file, state->line, state->idx, lua_tostring(L, -1));
		} else {
			logprintf(LOG_ERR, "Lua panic (#%d): %s", state->idx, lua_tostring(L, -1));
		}
		state->file = NULL;
		state->line = -1;
	} else {
		logprintf(LOG_ERR, "Lua panic: %s", lua_tostring(L, -1));
	}

	while(lua_gettop(L) > 0) {
		lua_remove(L, -1);
	}

	abort();
	return 0;
}

void plua_set_file_line(lua_State *L, char *file, int line) {
	struct lua_state_t *state = plua_get_current_state(L);
	state->line = line;
	state->file = file;
}

void plua_init(void) {
	if(init == 1) {
		return;
	}
	init = 1;

	uv_sem_init(&sem_used_states, 0);

	int i = 0;
	for(i=0;i<NRLUASTATES+1;i++) {
		memset(&lua_state[i], 0, sizeof(struct lua_state_t));
		uv_mutex_init(&lua_state[i].lock);
		uv_mutex_init(&lua_state[i].gc.lock);

		lua_State *L = luaL_newstate();

		luaL_openlibs(L);
		plua_register_library(L);
		lua_state[i].L = L;
		lua_state[i].idx = i;
		lua_state[i].file = NULL;
		lua_state[i].line = -1;

		lua_atpanic(L, &plua_atpanic);
#ifdef PILIGHT_UNITTEST
		lua_sethook(L, hook, LUA_MASKLINE, 0);
#endif
	}

	plua_override_global("pairs", luaB_pairs);
	plua_override_global("ipairs", luaB_ipairs);
	plua_override_global("next", luaB_next);

	/*
	 * Initialize global state garbage collector
	 */
	i--;
	memset(&lua_state[i], 0, sizeof(struct lua_state_t));
	uv_mutex_init(&lua_state[i].gc.lock);
}

int plua_module_exists(char *module, int type) {
	struct lua_state_t *state = plua_get_free_state();
	struct lua_State *L = NULL;

	if(state == NULL) {
		return 1;
	}
	if((L = state->L) == NULL) {
		plua_clear_state(state);
		return 1;
	}

	char name[512] = { '\0' }, *p = name;
	memset(name, '\0', 512);

	struct plua_module_t mod;
	struct plua_module_t *a = &mod;
	a->type = type;
	strcpy(a->name, module);

	plua_namespace(a, p);

	lua_getglobal(L, name);
	if(lua_type(L, -1) == LUA_TNIL) {
		lua_pop(L, -1);
		assert(plua_check_stack(L, 0) == 0);
		plua_clear_state(state);
		return -1;
	}
	if(lua_type(L, -1) != LUA_TTABLE) {
		lua_pop(L, -1);
		assert(plua_check_stack(L, 0) == 0);
		plua_clear_state(state);
		return -1;
	}
	lua_pop(L, -1);

	assert(plua_check_stack(L, 0) == 0);
	plua_clear_state(state);

	return 0;
}

void plua_gc_unreg(lua_State *L, void *ptr) {
	struct lua_state_t *state = NULL;

	if(L == NULL) {
		state = &lua_state[NRLUASTATES];
	} else {
		state = plua_get_current_state(L);
	}
	assert(state != NULL);

	uv_mutex_lock(&state->gc.lock);
	int i = 0;
	for(i=0;i<state->gc.nr;i++) {
		if(state->gc.list[i] != NULL) {
			if(state->gc.list[i]->free == 0 &&
				 state->gc.list[i]->ptr == ptr) {
				memset(state->gc.list[i], 0, sizeof(**state->gc.list));
				state->gc.list[i]->free = 1;
				break;
			}
		}
	}
	uv_mutex_unlock(&state->gc.lock);
}

void plua_gc_reg(lua_State *L, void *ptr, void (*callback)(void *ptr)) {
	struct lua_state_t *state = NULL;

	if(L == NULL) {
		state = &lua_state[NRLUASTATES];
	} else {
		state = plua_get_current_state(L);
	}
	assert(state != NULL);

	uv_mutex_lock(&state->gc.lock);
	int slot = -1, i = 0;
	for(i=0;i<state->gc.nr;i++) {
		if(state->gc.list[i]->free == 1) {
			state->gc.list[i]->free = 0;
			slot = i;
			break;
		}
	}

	if(slot == -1) {
		if(state->gc.size <= state->gc.nr) {
			if((state->gc.list = REALLOC(state->gc.list, sizeof(**state->gc.list)*(state->gc.size+12))) == NULL) {
				OUT_OF_MEMORY
			}
			memset(&state->gc.list[state->gc.size], 0, sizeof(**state->gc.list)*12);
			state->gc.size += 12;
		}
		if(state->gc.list[state->gc.nr] == NULL) {
			if((state->gc.list[state->gc.nr] = MALLOC(sizeof(**state->gc.list))) == NULL) {
				OUT_OF_MEMORY
			}
		}
		slot = state->gc.nr++;
	}
	memset(state->gc.list[slot], 0, sizeof(**state->gc.list));

	state->gc.list[slot]->ptr = ptr;
	state->gc.list[slot]->callback = callback;

	uv_mutex_unlock(&state->gc.lock);
}

static unsigned int number2bitwise(unsigned int num) {
	switch(num) {
		case LUA_TNONE: { return PLUA_TNONE; } break;
		case LUA_TNIL: { return PLUA_TNIL; } break;
		case LUA_TBOOLEAN: { return PLUA_TBOOLEAN; } break;
		case LUA_TLIGHTUSERDATA: { return PLUA_TLIGHTUSERDATA; } break;
		case LUA_TNUMBER: { return PLUA_TNUMBER; } break;
		case LUA_TSTRING: { return PLUA_TSTRING; } break;
		case LUA_TTABLE: { return PLUA_TTABLE; } break;
		case LUA_TFUNCTION: { return PLUA_TFUNCTION; } break;
		case LUA_TUSERDATA: { return PLUA_TUSERDATA; } break;
		case LUA_TTHREAD: { return PLUA_TTHREAD; } break;
	}
	return -1;
};

int plua_check_stack(lua_State *L, int numargs, ...) {
	struct lua_state_t *state = plua_get_current_state(L);
	unsigned int val = 0, i = 0, a = 0, b = 0;
	va_list ap;

	if(lua_gettop(L) != numargs || lua_gettop(L) < 0) {
		plua_stack_dump(L);
		if(state->oldmod != NULL) {
			logprintf(LOG_ERR, "%s:%s %s #%d\n",
				state->oldmod->name, state->oldmod->file, state->oldmod->btfile, state->oldmod->btline
			);
		}
		return -1;
	}

	va_start(ap, numargs);
	for(i=0;i<numargs;i++) {
		val = va_arg(ap, int);
		a = number2bitwise(lua_type(L, i+1));
		b = val;
		if((b & a) != a) {
			plua_stack_dump(L);
			if(state->oldmod != NULL) {
				logprintf(LOG_ERR, "%:%s %s #%d\n",
					state->oldmod->name, state->oldmod->file, state->oldmod->btfile, state->oldmod->btline
				);
			}
			return -1;
		}
	}
	va_end(ap);

	return 0;

	return 0;
}

//#ifdef PILIGHT_UNITTEST
void plua_override_global(char *name, int (*func)(lua_State *L)) {
	int i = 0;
	for(i=0;i<NRLUASTATES;i++) {
		uv_mutex_lock(&lua_state[i].lock);

		lua_getglobal(lua_state[i].L, "_G");
		lua_pushcfunction(lua_state[i].L, func);
		lua_setfield(lua_state[i].L, -2, name);
		lua_remove(lua_state[i].L, -1);

		assert(plua_check_stack(lua_state[i].L, 0) == 0);

		uv_mutex_unlock(&lua_state[i].lock);
	}
}
//#endif

int plua_get_method(struct lua_State *L, char *file, char *method) {
#if LUA_VERSION_NUM <= 502
	lua_getfield(L, -1, method);
	if(lua_type(L, -1) != LUA_TFUNCTION) {
#else
	if(lua_getfield(L, -1, method) == 0) {
#endif
		logprintf(LOG_ERR, "%s: %s function missing", file, method);
		return -1;
	}
	return 0;
}

static int plua_error_handler(lua_State* L) {
	struct lua_state_t *state = plua_get_current_state(L);

	lua_Debug ar;
	int level = 0, p = 0;
	int buffer = 4096;
	char msg[buffer];
	memset(&msg, 0, buffer);

	const char *err = lua_tostring(L, -1);
	p += snprintf(&msg[p], buffer-p, "\n---- LUA STACKTRACE ----\n");
	p += snprintf(&msg[p], buffer-p, " error: %s\n", err);
	lua_pop(L, 1);

	if(state != NULL) {
		if(state->file != NULL) {
			p += snprintf(&msg[p], buffer-p, " file: %s #%d\n", state->file, state->line);
		}
		if(state->module != NULL && state->module->btfile != NULL) {
			p += snprintf(&msg[p], buffer-p, " module: %s #%d\n", state->module->btfile, state->module->btline);
		}
	}
	p += snprintf(&msg[p], buffer-p, "\n");
	while(lua_getstack(L, level, &ar) == 1) {
		lua_getinfo(L, "nSfLl", &ar);
		lua_pop(L, 1);
		p += snprintf(&msg[p], buffer-p, " [#%.3d] %s:%d ", level, ar.short_src, ar.currentline);

		if(ar.name != 0) {
			p += snprintf(&msg[p], buffer-p, " (%s %s)\n", ar.namewhat, ar.name);
		} else {
			p += snprintf(&msg[p], buffer-p, "\n");
		}
		level++;
	}

	p += snprintf(&msg[p], buffer-p, "\n");
	p += snprintf(&msg[p], buffer-p, " number of element on stack: %d\n", lua_gettop(L));
	p += snprintf(&msg[p], buffer-p, "\n");
	int top = lua_gettop(L), i = 0;

	for(i = 1; i <= top; i++) {
		int t = lua_type(L, i);
		switch(t) {
			case LUA_TSTRING:
				p += snprintf(&msg[p], buffer-p, " %d: '%s'", i, lua_tostring(L, i));
			break;
			case LUA_TBOOLEAN:
				p += snprintf(&msg[p], buffer-p, " %d: %s", i, lua_toboolean(L, i) ? "true" : "false");
			break;
			case LUA_TNUMBER:
				p += snprintf(&msg[p], buffer-p, " %d: %g", i, lua_tonumber(L, i));
			break;
			default:
				p += snprintf(&msg[p], buffer-p, " %d: %s", i, lua_typename(L, t));
			break;
		}
		p += snprintf(&msg[p], buffer-p, "\n");
	}
	p += snprintf(&msg[p], buffer-p, "\n---- LUA STACKTRACE ----");
	lua_pushstring(L, msg);
	return 1;
}

int plua_pcall(struct lua_State *L, char *file, int args, int ret) {
	int hpos = lua_gettop(L) - args;
	lua_pushcfunction(L, plua_error_handler);
	lua_insert(L, hpos);

	if(lua_pcall(L, args, ret, hpos) == LUA_ERRRUN) {
		if(lua_type(L, -1) == LUA_TNIL) {
			logprintf(LOG_ERR, "%s: syntax error", file);
			lua_remove(L, -1);
			lua_remove(L, -1);
			return -1;
		}
		if(lua_type(L, -1) == LUA_TSTRING) {
			logprintf(LOG_ERR, "%s", lua_tostring(L, -1));
			if(ret == LUA_MULTRET) {
				lua_remove(L, hpos);
				lua_remove(L, -1);
			} else {
				while(lua_gettop(L) > ret) {
					lua_remove(L, -1);
				}
			}
			return -1;
		}
	}

	lua_remove(L, hpos);

	return 0;
}

static void plua__json_to_table(struct lua_State *L, struct JsonNode *jnode) {
	struct JsonNode *jchild = json_first_child(jnode);

	lua_newtable(L);
	int i = 0;
	while(jchild) {
		if(jnode->tag == JSON_OBJECT) {
			lua_pushstring(L, jchild->key);
		} else {
			lua_pushnumber(L, ++i);
		}
		switch(jchild->tag) {
			case JSON_STRING:
				lua_pushstring(L, jchild->string_);
			break;
			case JSON_NUMBER:
				lua_pushnumber(L, jchild->number_);
			break;
			case JSON_ARRAY:
			case JSON_OBJECT:
				plua__json_to_table(L, jchild);
			break;
		}
		lua_settable(L, -3);
		jchild = jchild->next;
	}
}

void plua_metatable_to_json(struct plua_metatable_t *table, struct JsonNode **jnode) {
	if(table == NULL) {
		return;
	}

	int mkobject = 0, x = 0;
	for(x=0;x<table->nrvar;x++) {
		if(table->table[x].key.type_ == LUA_TSTRING) {
			mkobject = 1;
			break;
		}
	}

	if(mkobject == 1) {
		*jnode = json_mkobject();
	} else {
		*jnode = json_mkarray();
	}

	for(x=0;x<table->nrvar;x++) {
		if(table->table[x].key.type_ == LUA_TNUMBER) {
			if(mkobject == 1) {
				int len = 0;
				char *key = NULL, *val = NULL;
				len = snprintf(NULL, 0, "%g", (float)table->table[x].key.number_);
				if((key = MALLOC(len+1)) == NULL) {
					OUT_OF_MEMORY
				}
				memset(key, 0, len+1);
				len = snprintf(key, len+1, "%g", (float)table->table[x].key.number_);

				if(table->table[x].val.type_ == LUA_TSTRING) {
					json_append_member(*jnode, key, json_mkstring(table->table[x].val.string_));
				} else if(table->table[x].val.type_ == LUA_TBOOLEAN) {
					json_append_member(*jnode, key, json_mkbool((int)table->table[x].val.number_));
				} else if(table->table[x].val.type_ == LUA_TNUMBER) {
					len = snprintf(NULL, 0, "%g", (float)table->table[x].val.number_);
					if((val = MALLOC(len+1)) == NULL) {
						OUT_OF_MEMORY
					}
					memset(val, 0, len+1);
					len = snprintf(val, len+1, "%g", (float)table->table[x].val.number_);

					json_append_member(*jnode, key, json_mknumber(table->table[x].val.number_, nrDecimals(val)));
					FREE(val);
				} else if(table->table[x].val.type_ == LUA_TTABLE) {
					struct JsonNode *jchild = NULL;
					plua_metatable_to_json(table->table[x].val.void_, &jchild);
					json_append_member(*jnode, key, jchild);
				}

				FREE(key);
			} else {
				if(table->table[x].val.type_ == LUA_TSTRING) {
					json_append_element(*jnode, json_mkstring(table->table[x].val.string_));
				} else if(table->table[x].val.type_ == LUA_TBOOLEAN) {
					json_append_element(*jnode, json_mkbool((int)table->table[x].val.number_));
				} else if(table->table[x].val.type_ == LUA_TNUMBER) {
					char *val = NULL;
					int len = snprintf(NULL, 0, "%g", (float)table->table[x].val.number_);
					if((val = MALLOC(len+1)) == NULL) {
						OUT_OF_MEMORY
					}
					memset(val, 0, len+1);
					len = snprintf(val, len+1, "%g", (float)table->table[x].val.number_);

					json_append_element(*jnode, json_mknumber(table->table[x].val.number_, nrDecimals(val)));
					FREE(val);
				} else if(table->table[x].val.type_ == LUA_TTABLE) {
					struct JsonNode *jchild = NULL;
					plua_metatable_to_json(table->table[x].val.void_, &jchild);
					json_append_element(*jnode, jchild);
				}
			}
		} else if(table->table[x].key.type_ == LUA_TSTRING) {
			if(mkobject == 1) {
				if(table->table[x].val.type_ == LUA_TSTRING) {
					json_append_member(*jnode, table->table[x].key.string_, json_mkstring(table->table[x].val.string_));
				} else if(table->table[x].val.type_ == LUA_TBOOLEAN) {
					json_append_member(*jnode, table->table[x].key.string_, json_mkbool((int)table->table[x].val.number_));
				} else if(table->table[x].val.type_ == LUA_TNUMBER) {
					char *val = NULL;
					int len = snprintf(NULL, 0, "%g", (float)table->table[x].val.number_);
					if((val = MALLOC(len+1)) == NULL) {
						OUT_OF_MEMORY
					}
					memset(val, 0, len+1);
					len = snprintf(val, len+1, "%g", (float)table->table[x].val.number_);

					json_append_member(*jnode, table->table[x].key.string_, json_mknumber(table->table[x].val.number_, nrDecimals(val)));
					FREE(val);
				} else if(table->table[x].val.type_ == LUA_TTABLE) {
					struct JsonNode *jchild = NULL;
					plua_metatable_to_json(table->table[x].val.void_, &jchild);
					json_append_member(*jnode, table->table[x].key.string_, jchild);
				}
			} else {
				if(table->table[x].val.type_ == LUA_TSTRING) {
					json_append_element(*jnode, json_mkstring(table->table[x].val.string_));
				} else if(table->table[x].val.type_ == LUA_TBOOLEAN) {
					json_append_element(*jnode, json_mkbool((int)table->table[x].val.number_));
				} else if(table->table[x].val.type_ == LUA_TNUMBER) {
					char *val = NULL;
					int len = snprintf(NULL, 0, "%g", (float)table->table[x].val.number_);
					if((val = MALLOC(len+1)) == NULL) {
						OUT_OF_MEMORY
					}
					memset(val, 0, len+1);
					len = snprintf(val, len+1, "%g", (float)table->table[x].val.number_);

					json_append_element(*jnode, json_mknumber(table->table[x].val.number_, nrDecimals(val)));
					FREE(val);
				} else if(table->table[x].val.type_ == LUA_TTABLE) {
					struct JsonNode *jchild = NULL;
					plua_metatable_to_json(table->table[x].val.void_, &jchild);
					json_append_element(*jnode, jchild);
				}
			}
		}
	}
}

int plua_json_to_table(struct plua_metatable_t *table, struct JsonNode *jnode) {
	struct lua_state_t *state = plua_get_free_state();
	struct lua_State *L = NULL;

	if(state == NULL) {
		return -1;
	}

	L = state->L;

	plua__json_to_table(L, jnode);

	if(lua_type(L, -1) == LUA_TTABLE) {
		lua_pushnil(L);
		while(lua_next(L, -2) != 0) {
			plua_metatable_parse_set(L, table);
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
		assert(plua_check_stack(L, 0) == 0);
		plua_clear_state(state);

		return 0;
	}

	lua_pop(L, 1);
	assert(plua_check_stack(L, 0) == 0);
	plua_clear_state(state);

	return -1;
}

int plua_gc(void) {
	if(init == 0) {
		return -1;
	}
	struct plua_module_t *tmp = NULL;
	while(modules) {
		tmp = modules;
		FREE(tmp->bytecode);
		// if(tmp->table != NULL) {
			// plua_metatable_free(tmp->table);
		// }
		modules = modules->next;
		FREE(tmp);
	}

	int i = 0, x = 0, _free = 1;
	while(_free) {
		_free = 0;
		for(i=0;i<NRLUASTATES+1;i++) {
			if(uv_mutex_trylock(&lua_state[i].lock) == 0) {
				for(x=0;x<lua_state[i].gc.nr;x++) {
					if(lua_state[i].gc.list[x]->free == 0) {
						lua_state[i].gc.list[x]->callback(lua_state[i].gc.list[x]->ptr);
					}
					FREE(lua_state[i].gc.list[x]);
				}
				if(lua_state[i].gc.size > 0) {
					FREE(lua_state[i].gc.list);
				}
				lua_state[i].gc.nr = 0;
				lua_state[i].gc.size = 0;

				if(lua_state[i].L != NULL) {
					lua_gc(lua_state[i].L, LUA_GCCOLLECT, 0);
					lua_close(lua_state[i].L);
					lua_state[i].L = NULL;
				}
				uv_mutex_unlock(&lua_state[i].lock);
			} else {
				_free = 1;
			}
		}
	}

	init = 0;
	logprintf(LOG_DEBUG, "garbage collected lua library");
	return 0;
}

#ifdef PILIGHT_UNITTEST
int plua_flush_coverage(void) {
	int x = 0, i = 0;
	if(covfile != NULL) {
		FILE *fp = NULL;
		char *name = malloc(strlen(covfile)+6);
		if(name == NULL) {
			OUT_OF_MEMORY
		}
		strcpy(name, covfile);
		strcat(name, ".info");

		/* Overwrite config file with proper format */
		if((fp = fopen(name, "w")) == NULL) {
			logprintf(LOG_ERR, "cannot write coverage output: %s", name);
			return -1;
		}
		fseek(fp, 0L, SEEK_SET);

		for(i=0;i<nrcoverage;i++) {
			char resolved_path[PATH_MAX];
			(void)(realpath(&coverage[i]->file[1], resolved_path)+1);

			fprintf(fp, "TN:\n");
			fprintf(fp, "SF:%s\n", resolved_path);
			for(x=1;x<coverage[i]->nrfstat;x++) {
				if(coverage[i]->fstat[x] >= 0) {
					if(coverage[i]->functions[x] != NULL) {
						fprintf(fp, "FN:%d,%s\n", x, coverage[i]->functions[x]);
					} else {
						fprintf(fp, "FNDA:%d,%d\n", coverage[i]->fstat[x], x);
					}
				}
			}
			for(x=1;x<coverage[i]->nrfstat;x++) {
				if(coverage[i]->fstat[x] >= 0) {
					if(coverage[i]->functions[x] != NULL) {
						fprintf(fp, "FNDA:%d,%s\n", coverage[i]->fstat[x], coverage[i]->functions[x]);
						free(coverage[i]->functions[x]);
					} else {
						fprintf(fp, "FNDA:%d,%d\n", coverage[i]->fstat[x], x);
					}
				}
			}
			for(x=1;x<coverage[i]->nrlstat;x++) {
				if(coverage[i]->lstat[x] >= 0) {
					fprintf(fp, "DA:%d,%d\n", x, coverage[i]->lstat[x]);
				}
			}
			fprintf(fp, "end_of_record\n");

			if(coverage[i]->nrfunctions > 0) {
				free(coverage[i]->functions);
			}
			if(coverage[i]->nrfstat > 0) {
				free(coverage[i]->fstat);
			}
			if(coverage[i]->nrlstat > 0) {
				free(coverage[i]->lstat);
			}
			free(coverage[i]->file);
			if(coverage[i]->module != NULL) {
				free(coverage[i]->module);
			}
			free(coverage[i]);
		}
		if(coverage != NULL) {
			free(coverage);
		}
		fclose(fp);
		free(name);
		nrcoverage = 0;
	}
	return 0;
}
#endif

void plua_package_path(const char *path) {
	int i = 0;
	for(i=0;i<NRLUASTATES;i++) {
		lua_getglobal(lua_state[i].L, "package");
		lua_getfield(lua_state[i].L, -1, "path");
		const char *tmp = lua_tostring(lua_state[i].L, -1);
		int len = strlen(tmp)+strlen(path)+3;
		char *newpath = MALLOC(len);
		if(newpath == NULL) {
			OUT_OF_MEMORY
		}
		snprintf(newpath, len, "%s;%s", tmp, path);
		lua_pop(lua_state[i].L, 1);
		lua_pushstring(lua_state[i].L, newpath);
		lua_setfield(lua_state[i].L, -2, "path");

		lua_pop(lua_state[i].L, 1);
		assert(plua_check_stack(lua_state[i].L, 0) == 0);
		FREE(newpath);
	}
}
