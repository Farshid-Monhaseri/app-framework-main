/*
 Copyright (C) 2015-2020 IoT.bzh

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

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/types.h>

#include <json-c/json.h>

#include "utils-json.h"
#include "wgt-info.h"
#include "wgt-json.h"
#include "wgt-strings.h"
#include "verbose.h"

/*
 * This describes an action to be performed for a parameter of name.
 * The action takes a not null json object, the param matched and a given closure.
 */
struct paramaction
{
	const char *name;	/* the name of the parameter trigerring the action or null (mark end) */
	int (*action)(struct json_object *obj, const struct wgt_desc_param *param, void *closure); /* the action to perform or null (no action) */
	void *closure;		/* the closure to pass to the action */
};


/*
 * Apply the first matching 'actions' to each of the given 'params' and for the given json 'obj' (that shouldn't be null)
 * Returns 0 in case of success or -1 on error.
 */
static int apply_params(struct json_object *obj, const struct wgt_desc_param *params, const struct paramaction *actions)
{
	int rc;
	const struct paramaction *a;

	if (!obj) {
		/* obj can't be null */
		errno = EINVAL;
		rc = -1;
	} else {
		/* iterate over the params */
		rc = 0;
		while(params) {
			/* search the first match */
			for (a = actions ; a->name && strcmp(a->name, params->name) ; a++);
			/* invoke the action (or not if null) */
			if (a->action && a->action(obj, params, a->closure) < 0)
				rc = -1;
			/* next */
			params = params->next;
		}
	}
	return rc;
}

/*
 * Get the array at the 'mkey' in 'obj'. 'create' it if needed and requested.
 * Returns 0 in case of success or a negative number otherwise.
 */
static int get_array(struct json_object **result, struct json_object *obj, const char *mkey, int create)
{
	/* enter the multiple key */
	int rc = j_enter_m(&obj, &mkey, create);
	if (rc < 0)
		/* parent keys doesn't exist */
		*result = NULL;
	else if (!json_object_object_get_ex(obj, mkey, result)) {
		/* parent keys exist but not the final key */
		if (!create) {
			*result = NULL;
			rc = -ENOENT;
		} else {
			/* try to create the array */
			if (!(*result = j_add_new_array(obj, mkey)))
				rc = -ENOMEM;
		}
	}
	return rc;
}

/*
 * Returns the target name (#target) of the feature 'feat'.
 * Returns 'defval' if not found or NULL is duplicated
 */
static const char *get_target_name(const struct wgt_desc_feature *feat, const char *defval)
{
	const struct wgt_desc_param *param = feat->params;

	/* search the parameter of name '#target' */
	while (param && strcmp(param->name, string_sharp_target))
		param = param->next;
	if (param) {
		/* found, get itsd value */
		defval = param->value;

		/* check it is defined only one time */
		/* TODO: validate it in an other place? */
		param = param->next;
		while (param) {
			if (!strcmp(param->name, string_sharp_target)) {
				defval = NULL;
				break;
			}
			param = param->next;
		}
	}
	return defval;
}

/*
 * Search in 'array' the first object having a field of name 'key' and of string value 'val'
 * Returns NULL if not found
 */
static struct json_object *get_array_item_by_key(struct json_object *array, const char *key, const char *val)
{
	struct json_object *result, *k;
	int i, n;

	n = json_object_array_length(array);
	for (i = 0 ; i < n ; i++) {
		result = json_object_array_get_idx(array, i);
		if (result && json_object_object_get_ex(result, key, &k)
			&& json_object_get_type(k) == json_type_string
			&& !strcmp(json_object_get_string(k), val))
			return result;
	}
	return NULL;
}

/*
 * Get or create from the array 'targets' the structure for the target of 'name'
 * Should create if 'create' is not not otherwise just get.
 * Returns zero on success or a errno negative code
 */
static int get_target(struct json_object **result, struct json_object *targets, const char *name, int create)
{
	int rc;
	struct json_object *t;

	/* search in targets a structure of target name */
	t = get_array_item_by_key(targets, string_sharp_target, name);
	if (t) {
		/* found */
		if (!create)
			rc = 0;
		else {
			ERROR("duplicated target name: %s", name);
			t = NULL;
			rc = -EEXIST;
		}
	} else {
		/* not found */
		if (!create) {
			ERROR("target name not found: %s", name);
			rc = -ENOENT;
		} else {
			/* create a new value */
			t = j_add_new_object(targets, NULL);
			if (t && j_add_string(t, string_sharp_target, name))
				rc = 0;
			else {
				json_object_put(t);
				t = NULL;
				rc = -ENOMEM;
			}
		}
	}
	*result = t;
	return rc;
}

/* create the target value for the feature */
static int create_target(struct json_object *targets, const struct wgt_desc_feature *feat)
{
	const char *id;
	struct json_object *target;

	/* search within the feature the parameter naming the target */
	id = get_target_name(feat, NULL);
	if (id == NULL) {
		ERROR("target of feature %s is %s", feat->name, get_target_name(feat, feat->name) ? "repeated" : "missing");
		return -EINVAL;
	}

	/* create the target */
	return get_target(&target, targets, id, 1);
}

/*
 * Adds icon data to the target
 */
static int add_icon(struct json_object *target, const struct wgt_desc_icon *icon)
{
	int rc;
	struct json_object *object, *array;

	rc = get_array(&array, target, string_icon, 1);
	if (rc >= 0) {
		object = json_object_new_object();
		if(!object
		|| !j_add_string(object, string_src, icon->src)
		|| (icon->width > 0 && !j_add_integer(object, string_width, icon->width))
		|| (icon->height > 0 && !j_add_integer(object, string_height, icon->height))
		|| !j_add(array, NULL, object)) {
			json_object_put(object);
			rc = -ENOMEM;
		}
	}
	return rc;
}

/*
 * Creates the main target
 */
static int create_main_target(struct json_object *targets, const struct wgt_desc *desc)
{
	int rc;
	struct wgt_desc_icon *icon;
	struct json_object *target;

	/* create the target 'main' */
	rc = get_target(&target, targets, string_main, 1);

	/* add icons if any */
	icon = desc->icons;
	while (rc >= 0 && icon) {
		rc = add_icon(target, icon);
		icon = icon->next;
	}

	/* add content */
	if (rc >= 0)
		rc = j_add_many_strings_m(target,
			"content.src", desc->content_src,
			"content.type", desc->content_type,
			"content.encoding", desc->content_encoding,
			NULL) ? 0 : -errno;

	/* add other info */
	if (rc >= 0)
		if((desc->width && !j_add_integer(target, string_width, desc->width))
		|| (desc->height && !j_add_integer(target, string_height, desc->height))
		|| (desc->viewmodes && !j_add_string(target, string_viewmodes,  desc->viewmodes))
		|| (desc->defaultlocale && !j_add_string(target, string_defaultlocale,  desc->defaultlocale)))
			rc = -ENOMEM;

	return rc;
}

/***********************************************************************************************************/

/*
 * translate a param to an object { "name": name, "value": value }
 */
static struct json_object *object_of_param(const struct wgt_desc_param *param)
{
	struct json_object *value;

	value = json_object_new_object();
	if (value
	 && j_add_string(value, string_name, param->name)
	 && j_add_string(value, string_value, param->value))
		return value;

	json_object_put(value);
	return NULL;
}

/*
 * Add the field of mkey 'param'.name with 'param'.value to the object 'obj'
 */
static int add_param_simple(struct json_object *obj, const struct wgt_desc_param *param, void *closure)
{
	return j_add_string_m(obj, param->name, param->value);
}

/* add a param object to an array of param objects */
static int add_param_array(struct json_object *obj, const struct wgt_desc_param *param, void *closure)
{
	const char *array_name = closure;
	struct json_object *array, *value;

	/* the array is either pointed by 'closure=array_name' or the given 'obj' */
	if (!array_name)
		array = obj;
	else if (!json_object_object_get_ex(obj, array_name, &array)) {
		array = j_add_new_array(obj, array_name);
		if (!array)
			return -ENOMEM;
	}

	/* append the param object */
	value = object_of_param(param);
	if (value && j_add(array, NULL, value))
		return 0;

	json_object_put(value);
	return -ENOMEM;
}

/* add a param object to an object of param objects, indexed by the param name */
static int add_param_object(struct json_object *obj, const struct wgt_desc_param *param, void *closure)
{
	const char *object_name = closure;
	struct json_object *object, *value;

	/* the object is either pointed by 'object_name=closure' or the given 'obj' */
	if (!object_name)
		object = obj;
	else if (!json_object_object_get_ex(obj, object_name, &object)) {
		object = j_add_new_object(obj, object_name);
		if (!object)
			return -ENOMEM;
	}

	/* append the param object */
	value = object_of_param(param);
	if (value && j_add(object, param->name, value))
		return 0;

	json_object_put(value);
	return -ENOMEM;
}

/* Retrieve within 'targets' the target of the feature 'feat' and applies 'actions' to it */
static int add_targeted_params(struct json_object *targets, const struct wgt_desc_feature *feat, struct paramaction actions[])
{
	int rc;
	const char *id;
	struct json_object *obj;

	id = get_target_name(feat, string_main);
	rc = get_target(&obj, targets, id, 0);
	return rc < 0 ? rc : apply_params(obj, feat->params, actions);
}

/* Treats the feature "provided_unit" */
static int add_provided_unit(struct json_object *targets, const struct wgt_desc_feature *feat)
{
	static struct paramaction actions[] = {
		{ .name = string_sharp_target, .action = NULL, .closure = NULL }, /* skip #target */
		{ .name = NULL, .action = add_param_simple, .closure = NULL }
	};
	return add_targeted_params(targets, feat, actions);
}

/* Treats the feature "provided_api" */
static int add_provided_api(struct json_object *targets, const struct wgt_desc_feature *feat)
{
	static struct paramaction actions[] = {
		{ .name = string_sharp_target, .action = NULL, .closure = NULL }, /* skip #target */
		{ .name = NULL, .action = add_param_array, .closure = (void*)string_provided_api }
	};
	return add_targeted_params(targets, feat, actions);
}

/* Treats the feature "required_api" */
static int add_required_api(struct json_object *targets, const struct wgt_desc_feature *feat)
{
	static struct paramaction actions[] = {
		{ .name = string_sharp_target, .action = NULL, .closure = NULL }, /* skip #target */
		{ .name = NULL, .action = add_param_array, .closure = (void*)string_required_api }
	};
	return add_targeted_params(targets, feat, actions);
}

/* Treats the feature "provided_binding" */
static int add_provided_binding(struct json_object *targets, const struct wgt_desc_feature *feat)
{
	static struct paramaction actions[] = {
		{ .name = string_sharp_target, .action = NULL, .closure = NULL }, /* TODO: should be an error! */
		{ .name = NULL, .action = add_param_array, .closure = (void*)string_provided_binding }
	};
	return add_targeted_params(targets, feat, actions);
}

/* Treats the feature "required_binding" */
static int add_required_binding(struct json_object *targets, const struct wgt_desc_feature *feat)
{
	static struct paramaction actions[] = {
		{ .name = string_sharp_target, .action = NULL, .closure = NULL }, /* skip #target */
		{ .name = NULL, .action = add_param_array, .closure = (void*)string_required_binding }
	};
	return add_targeted_params(targets, feat, actions);
}

/* Treats the feature "required_permission" */
static int add_required_permission(struct json_object *targets, const struct wgt_desc_feature *feat)
{
	static struct paramaction actions[] = {
		{ .name = string_sharp_target, .action = NULL, .closure = NULL }, /* skip #target */
		{ .name = NULL, .action = add_param_object, .closure = (void*)string_required_permission }
	};
	return add_targeted_params(targets, feat, actions);
}

/* Treats the feature "defined_permission" */
static int add_defined_permission(struct json_object *defperm, const struct wgt_desc_feature *feat)
{
	static struct paramaction actions[] = {
		{ .name = NULL, .action = add_param_array, .closure = NULL }
	};
	return apply_params(defperm, feat->params, actions);
}

/***********************************************************************************************************/

/*
 * Create the json object from 'desc'
 */
static struct json_object *to_json(const struct wgt_desc *desc)
{
	size_t prefixlen;
	const struct wgt_desc_feature *feat;
	const char *featname;
	struct json_object *result, *targets, *permissions;
	int rc, rc2;

	/* create the application structure */
	if(!(result = json_object_new_object())
	|| !(targets = j_add_new_array(result, string_targets))
	|| !(permissions = j_add_new_array(result, string_defined_permission))
	)
		goto error;

	/* first pass: declarations */
	rc = create_main_target(targets, desc);
	prefixlen = strlen(string_AGL_widget_prefix);
	for (feat = desc->features ; feat ; feat = feat->next) {
		featname = feat->name;
		if (!memcmp(featname, string_AGL_widget_prefix, prefixlen)) {
			if (!feat->required) {
				ERROR("feature %s can't be optional", featname);
				if (!rc)
					rc = -EINVAL;
			}
			featname += prefixlen;
			if (!strcmp(featname, string_provided_unit)) {
				rc2 = create_target(targets, feat);
				if (rc2 < 0 && !rc)
					rc = rc2;
			}
		}
	}

	/* second pass: definitions */
	for (feat = desc->features ; feat ; feat = feat->next) {
		featname = feat->name;
		if (!memcmp(featname, string_AGL_widget_prefix, prefixlen)) {
			featname += prefixlen;
			if (!strcmp(featname, string_defined_permission)) {
				rc2 = add_defined_permission(permissions, feat);
			}
			else if (!strcmp(featname, string_provided_unit)) {
				rc2 = add_provided_unit(targets, feat);
			}
			else if (!strcmp(featname, string_provided_api)) {
				rc2 = add_provided_api(targets, feat);
			}
			else if (!strcmp(featname, string_provided_binding)) {
				rc2 = add_provided_binding(targets, feat);
			}
			else if (!strcmp(featname, string_required_api)) {
				rc2 = add_required_api(targets, feat);
			}
			else if (!strcmp(featname, string_required_binding)) {
				rc2 = add_required_binding(targets, feat);
			}
			else if (!strcmp(featname, string_required_permission)) {
				rc2 = add_required_permission(targets, feat);
			} else {
				/* gently ignore other features */
				rc2 = 0;
			}
			if (rc2 < 0 && !rc)
				rc = rc2;
		}
	}

	/* fills the main */
	rc2 = j_add_many_strings_m(result,
		string_id, desc->id,
		string_idaver, desc->idaver,
		string_version, desc->version,
		"ver", desc->ver,
		"author.content", desc->author,
		"author.href", desc->author_href,
		"author.email", desc->author_email,
		"license.content", desc->license,
		"license.href", desc->license_href,
		"defaultlocale", desc->defaultlocale,
		"name.content", desc->name,
		"name.short", desc->name_short,
		"description", desc->description,
		NULL) ? 0 : -errno;
	if (rc2 < 0 && !rc)
		rc = rc2;

	/* returns the result if there is no error*/
	if (!rc)
		return result;

error:
	json_object_put(result);
	return NULL;
}

/* get the json_object of the wgt_info 'info' or NULL if error */
struct json_object *wgt_info_to_json(struct wgt_info *info)
{
	return to_json(wgt_info_desc(info));
}

/* get the json_object of the 'wgt' or NULL if error */
struct json_object *wgt_to_json(struct wgt *wgt)
{
	struct json_object *result;
	struct wgt_info *info;

	info = wgt_info_create(wgt, 1, 1, 1);
	if (info == NULL)
		result = NULL;
	else {
		result = wgt_info_to_json(info);
		wgt_info_unref(info);
	}
	return result;
}

/* get the json_object of the widget of 'path' relative to 'dfd' or NULL if error */
struct json_object *wgt_path_at_to_json(int dfd, const char *path)
{
	struct json_object *result;
	struct wgt_info *info;

	info = wgt_info_createat(dfd, path, 1, 1, 1);
	if (info == NULL)
		result = NULL;
	else {
		result = wgt_info_to_json(info);
		wgt_info_unref(info);
	}
	return result;
}

/* get the json_object of the widget of 'path' or NULL if error */
struct json_object *wgt_path_to_json(const char *path)
{
	return wgt_path_at_to_json(AT_FDCWD, path);
}


