/*
 Copyright 2015 IoT.bzh

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

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <pwd.h>
#include <sys/types.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>

#include <json.h>

#include "verbose.h"
#include "utils-dir.h"
#include "af-launch.h"

enum appstate {
	as_starting,
	as_running,
	as_stopped,
	as_terminating,
	as_terminated
};

struct apprun {
	struct apprun *next_by_runid;
	struct apprun *next_by_pgid;
	int runid;
	pid_t pids[2]; /* 0: group leader, 1: slave (appli) */
	enum appstate state;
	json_object *appli;
};

#define ROOT_RUNNERS_COUNT  32
#define MAX_RUNNER_COUNT    32767

static struct apprun *runners_by_runid[ROOT_RUNNERS_COUNT];
static struct apprun *runners_by_pgid[ROOT_RUNNERS_COUNT];
static int runnercount = 0;
static int runnerid = 0;

static const char fwk_user_app_dir[] = FWK_USER_APP_DIR;
static char *homeappdir;

/****************** manages pgids **********************/

/* get a runner by its pgid */
static struct apprun *runner_of_pgid(pid_t pgid)
{
	struct apprun *result = runners_by_pgid[(int)(pgid & (ROOT_RUNNERS_COUNT - 1))];
	while (result && result->pids[0] != pgid)
		result = result->next_by_pgid;
	return result;
}

/* insert a runner for its pgid */
static void pgid_insert(struct apprun *runner)
{
	struct apprun **prev = &runners_by_runid[(int)(runner->pids[0] & (ROOT_RUNNERS_COUNT - 1))];
	runner->next_by_pgid = *prev;
	*prev = runner;
}

/* remove a runner for its pgid */
static void pgid_remove(struct apprun *runner)
{
	struct apprun **prev = &runners_by_runid[(int)(runner->pids[0] & (ROOT_RUNNERS_COUNT - 1))];
	runner->next_by_pgid = *prev;
	*prev = runner;
}

/****************** manages pids **********************/

/* get a runner by its pid */
static struct apprun *runner_of_pid(pid_t pid)
{
	/* try avoiding system call */
	struct apprun *result = runner_of_pgid(pid);
	if (result == NULL) {
		result = runner_of_pgid(getpgid(pid));
		if (result && result->pids[1] != pid)
			result = NULL;
	}
	return result;
}

/****************** manages runners (by runid) **********************/

/* get a runner by its runid */
static struct apprun *getrunner(int runid)
{
	struct apprun *result = runners_by_runid[runid & (ROOT_RUNNERS_COUNT - 1)];
	while (result && result->runid != runid)
		result = result->next_by_runid;
	return result;
}

/* free an existing runner */
static void freerunner(struct apprun *runner)
{
	struct apprun **prev = &runners_by_runid[runner->runid & (ROOT_RUNNERS_COUNT - 1)];
	assert(*prev);
	while(*prev != runner) {
		prev = &(*prev)->next_by_runid;
		assert(*prev);
	}
	*prev = runner->next_by_runid;
	json_object_put(runner->appli);
	free(runner);
	runnercount--;
}

/* create a new runner */
static struct apprun *createrunner(json_object *appli)
{
	struct apprun *result;
	struct apprun **prev;

	if (runnercount >= MAX_RUNNER_COUNT) {
		errno = EAGAIN;
		return NULL;
	}
	do {
		runnerid++;
		if (runnerid > MAX_RUNNER_COUNT)
			runnerid = 1;
	} while(getrunner(runnerid));
	result = calloc(1, sizeof * result);
	if (result == NULL)
		errno = ENOMEM;
	else {
		prev = &runners_by_runid[runnerid & (ROOT_RUNNERS_COUNT - 1)];
		result->next_by_runid = *prev;
		result->next_by_pgid = NULL;
		result->runid = runnerid;
		result->pids[0] = result->pids[1] = 0;
		result->state = as_starting;
		result->appli = json_object_get(appli);
		*prev = result;
		runnercount++;
	}
	return result;
}

/**************** signaling ************************/
#if 0
static void started(int runid)
{
}

static void stopped(int runid)
{
}

static void continued(int runid)
{
}

static void terminated(int runid)
{
}

static void removed(int runid)
{
}
#endif
/**************** running ************************/

static int killrunner(int runid, int sig, enum appstate tostate)
{
	int rc;
	struct apprun *runner = getrunner(runid);
	if (runner == NULL) {
		errno = ENOENT;
		rc = -1;
	}
	else if (runner->state != as_running && runner->state != as_stopped) {
		errno = EPERM;
		rc = -1;
	}
	else if (runner->state == tostate) {
		rc = 0;
	}
	else {
		rc = killpg(runner->pids[0], sig);
		if (!rc)
			runner->state = tostate;
	}
	return rc;
}

static void on_sigchld(int signum, siginfo_t *info, void *uctxt)
{
	struct apprun *runner;

	runner = runner_of_pgid(info->si_pid);
	if (!runner)
		return;

	switch(info->si_code) {
	case CLD_EXITED:
	case CLD_KILLED:
	case CLD_DUMPED:
	case CLD_TRAPPED:
		runner->state = as_terminated;
		pgid_remove(runner);
		break;

	case CLD_STOPPED:
		runner->state = as_stopped;
		break;

	case CLD_CONTINUED:
		runner->state = as_running;
		break;
	}
}

/**************** handle af_launch_desc *********************/

static int get_jstr(struct json_object *obj, const char *key, const char **value)
{
	json_object *data;
	return json_object_object_get_ex(obj, key, &data)
		&& json_object_get_type(data) == json_type_string
		&& (*value = json_object_get_string(data)) != NULL;
}

static int get_jint(struct json_object *obj, const char *key, int *value)
{
	json_object *data;
	return json_object_object_get_ex(obj, key, &data)
		&& json_object_get_type(data) == json_type_int
		&& ((*value = (int)json_object_get_int(data)), 1);
}

static int fill_launch_desc(struct json_object *appli, struct af_launch_desc *desc)
{
	json_object *pub;

	/* main items */
	if(!json_object_object_get_ex(appli, "public", &pub)
	|| !get_jstr(appli, "path", &desc->path)
	|| !get_jstr(appli, "id", &desc->tag)
	|| !get_jstr(appli, "content", &desc->content)
	|| !get_jstr(appli, "type", &desc->type)
	|| !get_jstr(pub, "name", &desc->name)
	|| !get_jint(pub, "width", &desc->width)
	|| !get_jint(pub, "height", &desc->height)) {
		errno = EINVAL;
		return -1;
	}

	/* plugins */
	{
		/* TODO */
		static const char *null = NULL;
		desc->plugins = &null;
	}

	/* finaly */
	desc->home = homeappdir;
	return 0;
};

/**************** API handling ************************/

int af_run_start(struct json_object *appli)
{
	static struct apprun *runner;
	struct af_launch_desc desc;
	int rc;
	sigset_t saved, blocked;

	/* prepare to launch */
	rc = fill_launch_desc(appli, &desc);
	if (rc)
		return rc;
	runner = createrunner(appli);
	if (!runner)
		return -1;

	/* block children signals until launched */
	sigemptyset(&blocked);
	sigaddset(&blocked, SIGCHLD);
	sigprocmask(SIG_BLOCK, &blocked, &saved);

	/* launch now */
	rc = af_launch(&desc, runner->pids);
	if (rc < 0) {
		/* fork failed */
		sigprocmask(SIG_SETMASK, &saved, NULL);
		ERROR("can't start, af_launch failed: %m");
		freerunner(runner);
		return -1;
	}

	/* insert the pid */
	runner->state = as_running;
	pgid_insert(runner);
	rc = runner->runid;

	/* unblock children signal now */
	sigprocmask(SIG_SETMASK, &saved, NULL);
	return rc;
}

int af_run_terminate(int runid)
{
	return killrunner(runid, SIGTERM, as_terminating);
}

int af_run_stop(int runid)
{
	return killrunner(runid, SIGSTOP, as_stopped);
}

int af_run_continue(int runid)
{
	return killrunner(runid, SIGCONT, as_running);
}

static json_object *mkstate(struct apprun *runner)
{
	const char *state;
	struct json_object *result, *obj;
	int rc;

	/* the structure */
	result = json_object_new_object();
	if (result == NULL)
		goto error;

	/* the runid */
	obj = json_object_new_int(runner->runid);
	if (obj == NULL)
		goto error2;
	json_object_object_add(result, "runid", obj); /* TODO TEST STATUS */

	/* the state */
	switch(runner->state) {
	case as_starting:
	case as_running:
		state = "running";
		break;
	case as_stopped:
		state = "stopped";
		break;
	default:
		state = "terminated";
		break;
	}
	obj = json_object_new_string(state);
	if (obj == NULL)
		goto error2;
	json_object_object_add(result, "state", obj); /* TODO TEST STATUS */

	/* the application id */
	rc = json_object_object_get_ex(runner->appli, "public", &obj);
	assert(rc);
	rc = json_object_object_get_ex(obj, "id", &obj);
	assert(rc);
	json_object_object_add(result, "id", obj); /* TODO TEST STATUS */
	json_object_get(obj);

	/* done */
	return result;

error2:
	json_object_put(result);
error:
	errno = ENOMEM;
	return NULL;
}

struct json_object *af_run_list()
{
	struct json_object *result, *obj;
	struct apprun *runner;
	int i;

	/* creates the object */
	result = json_object_new_array();
	if (result == NULL) {
		errno = ENOMEM;
		return NULL;		
	}

	for (i = 0 ; i < ROOT_RUNNERS_COUNT ; i++) {
		for (runner = runners_by_runid[i] ; runner ; runner = runner->next_by_runid) {
			if (runner->state != as_terminating && runner->state != as_terminated) {
				obj = mkstate(runner);
				if (obj == NULL) {
					json_object_put(result);
					return NULL;
				}
				/* TODO status ? */
				json_object_array_add(result, obj);
			}
		}
	}
	return result;
}

struct json_object *af_run_state(int runid)
{
	struct apprun *runner = getrunner(runid);
	if (runner == NULL || runner->state == as_terminating || runner->state == as_terminated) {
		errno = ENOENT;
		return NULL;
	}
	return mkstate(runner);
}

/**************** INITIALISATION **********************/

int af_run_init()
{
	char buf[2048];
	char dir[PATH_MAX];
	int rc;
	uid_t me;
	struct passwd passwd, *pw;
	struct sigaction siga;

	/* computes the 'homeappdir' */
	me = geteuid();
	rc = getpwuid_r(me, &passwd, buf, sizeof buf, &pw);
	if (rc || pw == NULL) {
		errno = rc ? errno : ENOENT;
		ERROR("getpwuid_r failed for uid=%d: %m",(int)me);
		return -1;
	}
	rc = snprintf(dir, sizeof dir, "%s/%s", passwd.pw_dir, fwk_user_app_dir);
	if (rc >= sizeof dir) {
		ERROR("buffer overflow in user_app_dir for uid=%d",(int)me);
		return -1;
	}
	rc = create_directory(dir, 0755, 1);
	if (rc && errno != EEXIST) {
		ERROR("creation of directory %s failed in user_app_dir: %m", dir);
		return -1;
	}
	homeappdir = strdup(dir);
	if (homeappdir == NULL) {
		errno = ENOMEM;
		ERROR("out of memory in user_app_dir for %s : %m", dir);
		return -1;
	}

	/* install signal handlers */
	siga.sa_flags = SA_SIGINFO | SA_NOCLDWAIT;
	sigemptyset(&siga.sa_mask);
	sigaddset(&siga.sa_mask, SIGCHLD);
	siga.sa_sigaction = on_sigchld;
	sigaction(SIGCHLD, &siga, NULL);
	return 0;
}

