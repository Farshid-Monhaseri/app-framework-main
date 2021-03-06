/*
 Author: José Bollo <jobol@nonadev.net>
 Author: José Bollo <jose.bollo@iot.bzh>

 https://gitlab.com/jobol/mustach

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

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <json-c/json.h>

#include "mustach.h"
#include "verbose.h"

#define MAX_DEPTH 256

/*
 * exploration state when instantiating mustache
 */
struct expl {
	struct json_object *root;
	int depth;
	struct {
		struct json_object *cont;
		struct json_object *obj;
		int index, count;
	} stack[MAX_DEPTH];
};

/*
 * Scan a key=val text.
 * If the sign = is found, drop it and returns a pointer to the value.
 * If the sign = is not here, returns NULL.
 * Replace any \= of the key by its unescaped version =.
 */
static char *keyval(char *read, int isptr)
{
	char *write, c;

	c = *(write = read);
	while (c && c != '=') {
		if (isptr) {
			if (c == '~' && read[1] == '=') {
				c = *++read;
			}
		} else {
			if (c == '\\') {
				switch (read[1]) {
				case '\\': *write++ = c; /*@fallthrough@*/
				case '=': c = *++read; /*@fallthrough@*/
				default: break;
				}
			}
		}
		*write++ = c;
		c = *++read;
	}
	*write = 0;
	return c == '=' ? ++read : NULL;
}

/*
 * Returns the unescaped version of the first component
 * and update 'name' to point the next components if any.
 */
static char *first(char **name, int isptr)
{
	char *r, *read, *write, c;

	c = *(read = *name);
	if (!c)
		r = NULL;
	else {
		r = write = read;
		if (isptr) {
			while (c && c != '/') {
				if (c == '~') {
					switch(read[1]) {
					case '1': c = '/'; /*@fallthrough@*/
					case '0': read++; /*@fallthrough@*/
					default: break;
					}
				}
				*write++ = c;
				c = *++read;
			}
		} else {
			while (c && c != '.') {
				if (c == '\\' && (read[1] == '.' || read[1] == '\\'))
					c = *++read;
				*write++ = c;
				c = *++read;
			}
		}
		*write = 0;
		*name = read + !!c;
	}
	return r;
}

/*
 * Returns the unescaped version of the first value
 * and update 'val' to point the next value if any.
 */
static char *value(char **val)
{
	char *r, *read, *write, c;

	c = *(read = *val);
	if (!c)
		r = NULL;
	else {
		r = write = read;
		while (c && c != '|') {
			if (c == '\\' && (read[1] == '|' || read[1] == '\\'))
				c = *++read;
			*write++ = c;
			c = *++read;
		}
		*write = 0;
		*val = read + !!c;
	}
	return r;
}

/*
 * Replace the last occurence of ':' followed by
 * any character not being '*' by ':*', the
 * globalisation of the key.
 * Returns NULL if no globalisation is done
 * or else the key globalized.
 */
static char *globalize(char *key)
{
	char *f, *r;

	f = NULL;
	for (r = key; *r ; r++) {
		if (r[0] == ':' && r[1] && r[1] != '*')
			f = r;
	}
	if (f) {
		f[1] = '*';
		f[2] = 0;
		f = key;
	}
	return f;
}

/*
 * find the object of 'name'
 */
static struct json_object *find(struct expl *e, const char *name)
{
	int i, isptr;
	struct json_object *o, *r;
	char *n, *c, *v;

	/* get a local key */
	n = strdupa(name);

	/* is it a JSON pointer? */
	isptr = n[0] == '/';
	n += isptr;

	/* extract its value */
	v = keyval(n, isptr);

	/* search the first component for each valid globalisation */
	i = e->depth;
	c = first(&n, isptr);
	while (c) {
		if (i < 0) {
			/* next globalisation */
			i = e->depth;
			c = globalize(c);
		}
		else if (json_object_object_get_ex(e->stack[i].obj, c, &o)) {

			/* found the root, search the subcomponents */
			c = first(&n, isptr);
			while(c) {
				while (!json_object_object_get_ex(o, c, &r)) {
					c = globalize(c);
					if (!c)
						return NULL;
				}
				o = r;
				c = first(&n, isptr);
			}

			/* check the value if requested */
			if (v) {
				i = v[0] == '!';
				v += i;
				do {
					c = value(&v);
				} while (c && strcmp(c, json_object_get_string(o)));
				if (i != !c)
					o = NULL;
			}
			return o;
		}
		i--;
	}
	return NULL;
}

static int start(void *closure)
{
	struct expl *e = closure;
	e->depth = 0;
	e->stack[0].cont = NULL;
	e->stack[0].obj = e->root;
	e->stack[0].index = 0;
	e->stack[0].count = 1;
	return 0;
}

static void print(FILE *file, const char *string, int escape)
{
	if (!escape)
		fputs(string, file);
	else if (*string)
		do {
			switch(*string) {
			case '%': fputs("%%", file); break;
			case '\n': fputs("\\n\\\n", file); break;
			default: putc(*string, file); break;
			}
		} while(*++string);
}

static int put(void *closure, const char *name, int escape, FILE *file)
{
	struct expl *e = closure;
	struct json_object *o = find(e, name);
	if (o)
		print(file, json_object_get_string(o), escape);
	return 0;
}

static int enter(void *closure, const char *name)
{
	struct expl *e = closure;
	struct json_object *o = find(e, name);
	if (++e->depth >= MAX_DEPTH)
		return MUSTACH_ERROR_TOO_DEPTH;
	if (json_object_is_type(o, json_type_array)) {
		e->stack[e->depth].count = json_object_array_length(o);
		if (e->stack[e->depth].count == 0) {
			e->depth--;
			return 0;
		}
		e->stack[e->depth].cont = o;
		e->stack[e->depth].obj = json_object_array_get_idx(o, 0);
		e->stack[e->depth].index = 0;
	} else if (json_object_is_type(o, json_type_object) || json_object_get_boolean(o)) {
		e->stack[e->depth].count = 1;
		e->stack[e->depth].cont = NULL;
		e->stack[e->depth].obj = o;
		e->stack[e->depth].index = 0;
	} else {
		e->depth--;
		return 0;
	}
	return 1;
}

static int next(void *closure)
{
	struct expl *e = closure;
	if (e->depth <= 0)
		return MUSTACH_ERROR_CLOSING;
	e->stack[e->depth].index++;
	if (e->stack[e->depth].index >= e->stack[e->depth].count)
		return 0;
	e->stack[e->depth].obj = json_object_array_get_idx(e->stack[e->depth].cont, e->stack[e->depth].index);
	return 1;
}

static int leave(void *closure)
{
	struct expl *e = closure;
	if (e->depth <= 0)
		return MUSTACH_ERROR_CLOSING;
	e->depth--;
	return 0;
}

static struct mustach_itf itf = {
	.start = start,
	.put = put,
	.enter = enter,
	.next = next,
	.leave = leave
};

/*
 * Apply the object 'root' to the mustache 'template'.
 * In case of success, the function returns 0, the pointer
 * 'result' receives the allocated instanciation and
 * the pointer 'size' its size. Note that the real size
 * is one byte more to effectively store the terminating
 * null.
 * In case of error, it returns a negative error code.
 */
int apply_mustach(const char *template, struct json_object *root, char **result, size_t *size)
{
	int rc;
	struct expl e;

	e.root = root;
	rc = mustach(template, &itf, &e, result, size);
	if (rc < 0) {
		static const char *msgs[] = {
			"SYSTEM",
			"UNEXPECTED_END",
			"EMPTY_TAG",
			"TAG_TOO_LONG",
			"BAD_SEPARATORS",
			"TOO_DEPTH",
			"CLOSING",
			"BAD_UNESCAPE_TAG"
		};

		rc = -(rc + 1);
		ERROR("mustach error found: MUSTACH_ERROR_%s",
			rc < 0 || rc >= (int)(sizeof msgs / sizeof * msgs) ? "???" : msgs[rc]);
		rc = -1;
		errno = EINVAL;
	}
	return rc;
}

