/*
	Copyright (C) 2013 - 2016 CurlyMo

  This Source Code Form is subject to the terms of the Mozilla Public
  License, v. 2.0. If a copy of the MPL was not distributed with this
  file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

#ifndef _LUA_CONFIG_DEVICE_PROGRAM_H_
#define _LUA_CONFIG_DEVICE_PROGRAM_H_

#include "../../lua.h"
#include "../device.h"

int plua_config_device_program(lua_State *L, struct plua_device_t *dev);

#endif