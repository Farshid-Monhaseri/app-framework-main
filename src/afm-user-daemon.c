/*
 Copyright (C) 2015-2018 IoT.bzh

 author: José Bollo <jose.bollo@iot.bzh>

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
*/

#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <getopt.h>
#include <string.h>

#include <systemd/sd-bus.h>
#include <systemd/sd-event.h>
#include <json-c/json.h>

#include <afb/afb-ws-client.h>
#include <afb/afb-proto-ws.h>

#include "verbose.h"
#include "utils-jbus.h"
#include "utils-json.h"

#define AFM_USER_DBUS_PATH	"/org/AGL/afm/user"

/*
 * name of the application
 */
static const char appname[] = "afm-user-daemon";

/*
 * string for printing version
 */
static const char versionstr[] =
	"\n"
	"  %s  version="AFM_VERSION"\n"
	"\n"
	"  Copyright (C) 2015, 2016, 2017 \"IoT.bzh\"\n"
	"  AFB comes with ABSOLUTELY NO WARRANTY.\n"
	"  Licence Apache 2\n"
	"\n";

/*
 * string for printing usage
 */
static const char usagestr[] =
	"usage: %s [option(s)] afm-main-uri\n"
	"\n"
	"   -d           run as a daemon\n"
	"   -u addr      address of user D-Bus to use\n"
	"   -q           quiet\n"
	"   -v           verbose\n"
	"   -V           version\n"
	"\n";

/*
 * Option definition for getopt_long
 */
static const char options_s[] = "hdqvVu:";
static struct option options_l[] = {
	{ "user-dbus",   required_argument, NULL, 'u' },
	{ "daemon",      no_argument,       NULL, 'd' },
	{ "quiet",       no_argument,       NULL, 'q' },
	{ "verbose",     no_argument,       NULL, 'v' },
	{ "help",        no_argument,       NULL, 'h' },
	{ "version",     no_argument,       NULL, 'V' },
	{ NULL, 0, NULL, 0 }
};

/*
 * The methods propagated
 */
static const char *methods[] = {
	"runnables",
	"detail",
	"start",
	"once",
	"terminate",
	"pause",
	"resume",
	"stop",
	"continue",
	"runners",
	"state",
	"install",
	"uninstall",
	NULL
};

/*
 * Connections
 */
static struct sd_event *evloop;
static struct jbus *user_bus;
static struct afb_proto_ws *pws;
static char *sessionid;
static const char *uri;

/*
 * 
 */
static void on_pws_hangup(void *closure);
static void on_pws_reply_success(void *closure, void *request, struct json_object *result, const char *info);
static void on_pws_reply_fail(void *closure, void *request, const char *status, const char *info);
static void on_pws_event_broadcast(void *closure, const char *event_name, struct json_object *data);

/* the callback interface for pws */
static struct afb_proto_ws_client_itf pws_itf = {
	.on_reply_success = on_pws_reply_success,
	.on_reply_fail = on_pws_reply_fail,
	.on_event_broadcast = on_pws_event_broadcast,
};

static int try_connect_pws()
{
	pws = afb_ws_client_connect_api(evloop, uri, &pws_itf, NULL);
	if (pws == NULL) {
		fprintf(stderr, "connection to %s failed: %m\n", uri);
		return 0;
	}
	afb_proto_ws_on_hangup(pws, on_pws_hangup);
	return 1;
}

static void attempt_connect_pws(int count);

static int timehand(sd_event_source *s, uint64_t usec, void *userdata)
{
	sd_event_source_unref(s);
	attempt_connect_pws((int)(intptr_t)userdata);
	return 0;
}

static void attempt_connect_pws(int count)
{
	sd_event_source *s;
	if (!try_connect_pws()) {
		if (--count <= 0) {
			ERROR("Definitely disconnected");
			exit(1);
		}
		sd_event_add_time(evloop, &s, CLOCK_MONOTONIC, 5000000, 0, timehand, (void*)(intptr_t)count);
	}
}

static void on_pws_reply_success(void *closure, void *request, struct json_object *result, const char *info)
{
	struct sd_bus_message *smsg = request;
	jbus_reply_j(smsg, result);
}

static void on_pws_reply_fail(void *closure, void *request, const char *status, const char *info)
{
	struct sd_bus_message *smsg = request;
	jbus_reply_error_s(smsg, status);
}

static void on_pws_event_broadcast(void *closure, const char *event_name, struct json_object *data)
{
	jbus_send_signal_j(user_bus, "changed", data);
}

/* called when pws hangsup */
static void on_pws_hangup(void *closure)
{
	struct afb_proto_ws *apw = pws;
	pws = NULL;
	afb_proto_ws_unref(apw);
	attempt_connect_pws(10);
}

/* propagate the call to the service */
static void propagate(struct sd_bus_message *smsg, struct json_object *obj, void *closure)
{
	int rc;
	const char *verb = closure;

	INFO("method %s propagated for %s", verb, json_object_to_json_string(obj));
	if (!pws)
		jbus_reply_error_s(smsg, "disconnected");
	else {
		rc = afb_proto_ws_client_call(pws, verb, obj, sessionid, smsg);
		if (rc < 0)
			ERROR("calling %s(%s) failed: %m\n", verb, json_object_to_json_string(obj));
	} 
}

/*
 * Tiny routine to daemonize the program
 * Return 0 on success or -1 on failure.
 */
static int daemonize()
{
	int rc = fork();
	if (rc < 0)
		return rc;
	if (rc)
		_exit(0);
	return 0;
}

/*
 * Opens a sd-bus connection and returns it in 'ret'.
 * The sd-bus connexion is intended to be for user if 'isuser'
 * is not null. The adress is the default address when 'address'
 * is NULL or, otherwise, the given address.
 * It might be necessary to pass the address as an argument because
 * library systemd uses secure_getenv to retrieves the default
 * addresses and secure_getenv might return NULL in some cases.
 */
static int open_bus(sd_bus **ret, int isuser, const char *address)
{
	sd_bus *b;
	int rc;

	if (address == NULL)
		return (isuser ? sd_bus_default_user : sd_bus_default_system)(ret);

	rc = sd_bus_new(&b);
	if (rc < 0)
		return rc;

	rc = sd_bus_set_address(b, address);
	if (rc < 0)
		goto fail;

	sd_bus_set_bus_client(b, 1);

	rc = sd_bus_start(b);
	if (rc < 0)
		goto fail;

	*ret = b;
	return 0;

fail:
	sd_bus_unref(b);
	return rc;
}

/*
 * ENTRY POINT OF AFM-USER-DAEMON
 */
int main(int ac, char **av)
{
	int i, daemon = 0, rc;
	struct sd_bus *usrbus;
	const char *usr_bus_addr;
	const char **iter;

	LOGAUTH(appname);

	/* first interpretation of arguments */
	usr_bus_addr = NULL;
	while ((i = getopt_long(ac, av, options_s, options_l, NULL)) >= 0) {
		switch (i) {
		case 'h':
			printf(usagestr, appname);
			return 0;
		case 'V':
			printf(versionstr, appname);
			return 0;
		case 'q':
			if (verbosity)
				verbosity--;
			break;
		case 'v':
			verbosity++;
			break;
		case 'd':
			daemon = 1;
			break;
		case 'u':
			usr_bus_addr = optarg;
			break;
		case ':':
			ERROR("missing argument value");
			return 1;
		default:
			ERROR("unrecognized option");
			return 1;
		}
	}

	/* check argument count */
	if (optind >= ac) {
		ERROR("Uri to the framework is missing");
		return 1;
	}
	if (optind + 1 != ac) {
		ERROR("Extra parameters found");
		return 1;
	}
	uri = av[optind];

	/* init sessionid */
	asprintf(&sessionid, "%d-%s", (int)getuid(), appname);

	/* daemonize if requested */
	if (daemon && daemonize()) {
		ERROR("daemonization failed");
		return 1;
	}

	/* get systemd objects */
	rc = sd_event_new(&evloop);
	if (rc < 0) {
		ERROR("can't create event loop");
		return 1;
	}
	rc = open_bus(&usrbus, 1, usr_bus_addr);
	if (rc < 0) {
		ERROR("can't create user bus");
		return 1;
	}
	rc = sd_bus_attach_event(usrbus, evloop, 0);
	if (rc < 0) {
		ERROR("can't attach user bus to event loop");
		return 1;
	}

	/* connect to framework */
	if (!try_connect_pws()) {
		ERROR("connection to %s failed: %m\n", uri);
		return 1;
	}

	/* connect to the session bus */
	user_bus = create_jbus(usrbus, AFM_USER_DBUS_PATH);
	if (!user_bus) {
		ERROR("create_jbus failed");
		return 1;
	}

	/* init services */
	for (iter = methods ; *iter ; iter ++) {
		if (jbus_add_service_j(user_bus, *iter, propagate, (void*)*iter)) {
			ERROR("adding services failed");
			return 1;
		}
	}

	/* start servicing */
	if (jbus_start_serving(user_bus) < 0) {
		ERROR("can't start server");
		return 1;
	}

	/* run until error */
	for(;;)
		sd_event_run(evloop, (uint64_t)-1);
	return 0;
}

