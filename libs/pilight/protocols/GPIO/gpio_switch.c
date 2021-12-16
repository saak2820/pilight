/*
	Copyright (C) 2014 CurlyMo

	This file is part of pilight.

	pilight is free software: you can redistribute it and/or modify it under the
	terms of the GNU General Public License as published by the Free Software
	Foundation, either version 3 of the License, or (at your option) any later
	version.

	pilight is distributed in the hope that it will be useful, but WITHOUT ANY
	WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
	A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with pilight. If not, see	<http://www.gnu.org/licenses/>
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>
#ifndef _WIN32
#include <wiringx.h>
#endif
#include <assert.h>

#include "../../core/pilight.h"
#include "../../core/common.h"
#include "../../core/dso.h"
#include "../../core/log.h"
#include "../../core/irq.h"
#include "../../core/gc.h"
#include "../../config/settings.h"
#include "../protocol.h"
#include "gpio_switch.h"

#if !defined(__FreeBSD__) && !defined(_WIN32)

static unsigned short loop = 1;
static int threads = 0;

static void createMessage(int gpio, int state) {
	gpio_switch->message = json_mkobject();
	JsonNode *code = json_mkobject();
	json_append_member(code, "gpio", json_mknumber(gpio, 0));
	if(state) {
		json_append_member(code, "state", json_mkstring("on"));
	} else {
		json_append_member(code, "state", json_mkstring("off"));
	}

	json_append_member(gpio_switch->message, "message", code);
	json_append_member(gpio_switch->message, "origin", json_mkstring("receiver"));
	json_append_member(gpio_switch->message, "protocol", json_mkstring(gpio_switch->id));

	if(pilight.broadcast != NULL) {
		pilight.broadcast(gpio_switch->id, gpio_switch->message, PROTOCOL);
	}
	json_delete(gpio_switch->message);
	gpio_switch->message = NULL;
}

static void *thread(void *param) {
	struct protocol_threads_t *node = (struct protocol_threads_t *)param;
	struct JsonNode *json = (struct JsonNode *)node->param;
	struct JsonNode *jid = NULL;
	struct JsonNode *jchild = NULL;
	int id = 0, state = 0, nstate = 0;
	double itmp = 0.0;

	threads++;

	if((jid = json_find_member(json, "id"))) {
		jchild = json_first_child(jid);
		if(json_find_number(jchild, "gpio", &itmp) == 0) {
			id = (int)round(itmp);
			pinMode(id, PINMODE_INPUT);
			state = digitalRead(id);
		}
	}

	createMessage(id, state);

	while(loop) {
		nstate = digitalRead(id);
		if(nstate != state) {
			state = nstate;
			createMessage(id, state);
		}
		usleep(100000);
	}

	threads--;
	return (void *)NULL;
}

static struct threadqueue_t *initDev(JsonNode *jdevice) {
	struct lua_state_t *state = plua_get_free_state();
	char *platform = GPIO_PLATFORM;

	if(config_setting_get_string(state->L, "gpio-platform", 0, &platform) != 0) {
		logprintf(LOG_ERR, "no gpio-platform configured");
		assert(lua_gettop(state->L) == 0);
		plua_clear_state(state);
		return NULL;
	}
	assert(lua_gettop(state->L) == 0);
	plua_clear_state(state);
	if(strcmp(platform, "none") == 0) {
		FREE(platform);
		logprintf(LOG_ERR, "no gpio-platform configured");
		return NULL;
	}
	if(wiringXSetup(platform, logprintf1) < 0) {
		FREE(platform);
		return NULL;
	}
	FREE(platform);

	loop = 1;
	char *output = json_stringify(jdevice, NULL);
	JsonNode *json = json_decode(output);
	json_free(output);

	struct protocol_threads_t *node = protocol_thread_init(gpio_switch, json);
	return threads_register("gpio_switch", &thread, (void *)node, 0);
}

static int checkValues(struct JsonNode *jvalues) {
	double readonly = 0.0;
	char *platform = GPIO_PLATFORM;

	struct lua_state_t *state = plua_get_free_state();
	if(config_setting_get_string(state->L, "gpio-platform", 0, &platform) != 0) {
		logprintf(LOG_ERR, "no gpio-platform configured");
		assert(plua_check_stack(state->L, 0) == 0);
		plua_clear_state(state);
		return -1;
	}
	assert(lua_gettop(state->L) == 0);
	plua_clear_state(state);
	if(strcmp(platform, "none") == 0) {
		FREE(platform);
		logprintf(LOG_ERR, "no gpio-platform configured");
		return -1;
	}
	if(wiringXSetup(platform, logprintf1) < 0) {
		FREE(platform);
		return -1;
	}
	FREE(platform);

	struct JsonNode *jid = NULL;
	if((jid = json_find_member(jvalues, "id"))) {
		struct JsonNode *jchild = NULL;
		struct JsonNode *jchild1 = NULL;

		jchild = json_first_child(jid);
		while(jchild) {
			jchild1 = json_first_child(jchild);
			while(jchild1) {
				if(strcmp(jchild1->key, "gpio") == 0) {
					if(wiringXValidGPIO((int)round(jchild1->number_)) != 0) {
						return -1;
					}
				}
				jchild1 = jchild1->next;
			}
			jchild = jchild->next;
		}
	}

	if(json_find_number(jvalues, "readonly", &readonly) == 0) {
		if((int)readonly != 1) {
			return -1;
		}
	}

	return 0;
}

static void threadGC(void) {
	loop = 0;
	protocol_thread_stop(gpio_switch);
	while(threads > 0) {
		usleep(10);
	}
	protocol_thread_free(gpio_switch);
}
#endif

#if !defined(MODULE) && !defined(_WIN32)
__attribute__((weak))
#endif
void gpioSwitchInit(void) {

	protocol_register(&gpio_switch);
	protocol_set_id(gpio_switch, "gpio_switch");
	protocol_device_add(gpio_switch, "gpio_switch", "GPIO as a switch");
	gpio_switch->devtype = SWITCH;
	gpio_switch->hwtype = SENSOR;

	options_add(&gpio_switch->options, "t", "on", OPTION_NO_VALUE, DEVICES_STATE, JSON_STRING, NULL, NULL);
	options_add(&gpio_switch->options, "f", "off", OPTION_NO_VALUE, DEVICES_STATE, JSON_STRING, NULL, NULL);
	options_add(&gpio_switch->options, "g", "gpio", OPTION_HAS_VALUE, DEVICES_ID, JSON_NUMBER, NULL, "^([0-9]{1}|1[0-9]|20)$");

	options_add(&gpio_switch->options, "0", "readonly", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)1, "^[10]{1}$");
	options_add(&gpio_switch->options, "0", "confirm", OPTION_HAS_VALUE, GUI_SETTING, JSON_NUMBER, (void *)1, "^[10]{1}$");

#if !defined(__FreeBSD__) && !defined(_WIN32)
	gpio_switch->initDev=&initDev;
	gpio_switch->threadGC=&threadGC;
	gpio_switch->checkValues=&checkValues;
#endif
}

#if defined(MODULE) && !defined(_WIN32)
void compatibility(struct module_t *module) {
	module->name = "gpio_switch";
	module->version = "2.4";
	module->reqversion = "7.0";
	module->reqcommit = "186";
}

void init(void) {
	gpioSwitchInit();
}
#endif
