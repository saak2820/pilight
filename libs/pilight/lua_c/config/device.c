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

#ifndef _WIN32
	#include <unistd.h>
	#include <sys/time.h>
	#include <libgen.h>
	#include <dirent.h>
	#include <unistd.h>
#endif

#include "../../core/log.h"
#include "../config.h"
#include "../../config/config.h"
#include "../../protocols/protocol.h"
#include "devices/switch.h"
#include "devices/label.h"
#include "devices/dimmer.h"
#include "devices/screen.h"
#include "devices/relay.h"
#include "devices/datetime.h"
#include "devices/program.h"

static int plua_config_device_set_action_id(lua_State *L) {
	struct plua_device_t *dev = (void *)lua_topointer(L, lua_upvalueindex(1));

	if(dev == NULL) {
		pluaL_error(L, "internal error: device object not passed");
	}

	if(lua_gettop(L) != 0) {
		pluaL_error(L, "config getType requires 0 arguments, %d given", lua_gettop(L));
	}

	unsigned long id = event_action_set_execution_id(dev->name);

	lua_pushnumber(L, id);

	assert(plua_check_stack(L, 1, PLUA_TNUMBER) == 0);

	return 1;
}

static int plua_config_device_get_action_id(lua_State *L) {
	struct plua_device_t *dev = (void *)lua_topointer(L, lua_upvalueindex(1));

	if(dev == NULL) {
		pluaL_error(L, "internal error: device object not passed");
	}

	if(lua_gettop(L) != 0) {
		pluaL_error(L, "config getType requires 0 arguments, %d given", lua_gettop(L));
	}

	unsigned long id = 0;
	if(event_action_get_execution_id(dev->name, &id) == -1) {
		lua_pushnil(L);
	} else {
		lua_pushnumber(L, id);
	}

	assert(plua_check_stack(L, 1, PLUA_TNIL | PLUA_TNUMBER) == 0);

	return 1;
}

static int plua_config_device_get_name(lua_State *L) {
	struct plua_device_t *dev = (void *)lua_topointer(L, lua_upvalueindex(1));

	if(dev == NULL) {
		pluaL_error(L, "internal error: device object not passed");
	}

	if(lua_gettop(L) != 0) {
		pluaL_error(L, "config getName requires 0 arguments, %d given", lua_gettop(L));
	}

	lua_pushstring(L, dev->name);

	assert(plua_check_stack(L, 1, PLUA_TSTRING) == 0);

	return 1;
}

static int plua_config_device_get_type(lua_State *L) {
	struct plua_device_t *dev = (void *)lua_topointer(L, lua_upvalueindex(1));
	int x = 0;

	if(dev == NULL) {
		pluaL_error(L, "internal error: device object not passed");
	}

	if(lua_gettop(L) != 0) {
		pluaL_error(L, "config getType requires 0 arguments, %d given", lua_gettop(L));
	}

	lua_newtable(L);
	struct protocol_t *protocol = NULL;
	while(devices_select_protocol(0, (char *)dev->name, x++, &protocol) == 0) {
		lua_pushnumber(L, x);
		lua_pushnumber(L, protocol->devtype);
		lua_settable(L, -3);
	}

	assert(plua_check_stack(L, 1, PLUA_TTABLE) == 0);

	return 1;
}

static int plua_config_device_has_setting(lua_State *L) {
	struct plua_device_t *dev = (void *)lua_topointer(L, lua_upvalueindex(1));
	const char *setting = NULL;

	if(dev == NULL) {
		pluaL_error(L, "internal error: device object not passed");
	}

	if(lua_gettop(L) != 1) {
		pluaL_error(L, "config getType requires 1 arguments, %d given", lua_gettop(L));
	}

	char buf[128] = { '\0' }, *p = buf;
	char *error = "string expected, got %s";

	sprintf(p, error, lua_typename(L, lua_type(L, -1)));

	luaL_argcheck(L,
		(lua_type(L, -1) == LUA_TSTRING),
		1, buf);

	setting = lua_tostring(L, -1);
	lua_remove(L, -1);

	if(devices_select_number_setting(ORIGIN_ACTION, dev->name, (char *)setting, NULL, NULL) == 0 ||
		devices_select_string_setting(ORIGIN_ACTION, dev->name, (char *)setting, NULL) == 0) {
		lua_pushboolean(L, 1);

		assert(plua_check_stack(L, 1, PLUA_TBOOLEAN) == 0);

		return 1;
	}

	lua_pushboolean(L, 0);

	assert(plua_check_stack(L, 1, PLUA_TBOOLEAN) == 0);

	return 1;
}

static int plua_config_device_get_id(lua_State *L) {
	struct plua_device_t *dev = (void *)lua_topointer(L, lua_upvalueindex(1));
	int i = 0;

	if(dev == NULL) {
		pluaL_error(L, "internal error: device object not passed");
	}

	if(lua_gettop(L) != 0) {
		pluaL_error(L, "config getType requires 0 arguments, %d given", lua_gettop(L));
	}

	struct JsonNode *jrespond = NULL;
	struct JsonNode *jsetting = NULL;
	struct JsonNode *jvalues = NULL;
	struct JsonNode *jid = NULL;
	if(devices_select(0, dev->name, &jrespond) == 0) {
		if((jsetting = json_find_member(jrespond, "id")) != NULL && jsetting->tag == JSON_ARRAY) {
			lua_newtable(L);
			jid = json_first_child(jsetting);
			while(jid) {
				jvalues = json_first_child(jid);
				lua_pushnumber(L, ++i);
				lua_newtable(L);
				while(jvalues) {
					lua_pushstring(L, jvalues->key);
					if(jvalues->tag == JSON_STRING) {
						lua_pushstring(L, jvalues->string_);
					} else if(jvalues->tag == JSON_NUMBER) {
						lua_pushnumber(L, jvalues->number_);
					}
					lua_settable(L, -3);
					jvalues = jvalues->next;
				}
				jid = jid->next;
				lua_settable(L, -3);
			}
		}

		assert(plua_check_stack(L, 1, PLUA_TTABLE) == 0);

		return 1;
	}

	lua_pushnil(L);

	assert(plua_check_stack(L, 1, PLUA_TNIL) == 0);

	return 1;
}

static void plua_config_device_gc(void *ptr) {
	struct plua_device_t *dev  = ptr;
	FREE(dev->name);
	FREE(dev);
}

int plua_config_device(lua_State *L) {
	struct protocol_t *protocol = NULL;
	char buf[128] = { '\0' }, *p = buf;
	char *error = "string expected, got %s";
	const char *name = NULL;
	int x = 0;

	sprintf(p, error, lua_typename(L, lua_type(L, -1)));

	luaL_argcheck(L,
		(lua_type(L, -1) == LUA_TSTRING),
		1, buf);

	if(lua_type(L, -1) == LUA_TSTRING) {
		name = lua_tostring(L, -1);
		lua_remove(L, -1);
	}

	if(devices_select(0, (char *)name, NULL) != 0) {
		lua_pushnil(L);

		assert(plua_check_stack(L, 1, PLUA_TNIL) == 0);

		return 1;
	}

	struct plua_device_t *dev = MALLOC(sizeof(struct plua_device_t));
	if(dev == NULL) {
		OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
	}
	memset(dev, 0, sizeof(struct plua_device_t));
	if((dev->name = STRDUP((char *)name)) == NULL) {
		OUT_OF_MEMORY /*LCOV_EXCL_LINE*/
	}

	lua_newtable(L);

	lua_pushstring(L, "getName");
	lua_pushlightuserdata(L, dev);
	lua_pushcclosure(L, plua_config_device_get_name, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "getId");
	lua_pushlightuserdata(L, dev);
	lua_pushcclosure(L, plua_config_device_get_id, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "getType");
	lua_pushlightuserdata(L, dev);
	lua_pushcclosure(L, plua_config_device_get_type, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "hasSetting");
	lua_pushlightuserdata(L, dev);
	lua_pushcclosure(L, plua_config_device_has_setting, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "setActionId");
	lua_pushlightuserdata(L, dev);
	lua_pushcclosure(L, plua_config_device_set_action_id, 1);
	lua_settable(L, -3);

	lua_pushstring(L, "getActionId");
	lua_pushlightuserdata(L, dev);
	lua_pushcclosure(L, plua_config_device_get_action_id, 1);
	lua_settable(L, -3);

	while(devices_select_protocol(0, (char *)name, x++, &protocol) == 0) {
		switch(protocol->devtype) {
			case DATETIME:
				plua_config_device_datetime(L, dev);
			break;
			case DIMMER:
				plua_config_device_dimmer(L, dev);
			break;
			case SWITCH:
				plua_config_device_switch(L, dev);
			break;
			case RELAY:
				plua_config_device_relay(L, dev);
			break;
			case SCREEN:
				plua_config_device_screen(L, dev);
			break;
			case PENDINGSW:
				plua_config_device_program(L, dev);
			break;
			case LABEL:
				plua_config_device_label(L, dev);
			break;
		}
	}

	plua_gc_reg(L, (void *)dev, plua_config_device_gc);

	assert(plua_check_stack(L, 1, PLUA_TTABLE) == 0);

	return 1;
}