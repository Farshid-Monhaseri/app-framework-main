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

struct jreq;
struct jbus;
struct pollfd;

extern struct jbus *create_jbus(const char *path, int session);
extern struct jbus *create_jbus_session(const char *path);
extern struct jbus *create_jbus_system(const char *path);

extern void jbus_addref(struct jbus *jbus);
extern void jbus_unref(struct jbus *jbus);

extern int jbus_fill_pollfds(
		struct jbus **jbuses,
		int njbuses,
		struct pollfd *fds);

extern int jbus_dispatch_pollfds(
		struct jbus **jbuses,
		 int njbuses,
		 struct pollfd *fds,
		 int maxcount);

extern int jbus_read_write_dispatch_multiple(
		struct jbus **jbuses,
		int njbuses,
		int toms,
		int maxcount);

extern int jbus_dispatch_multiple(
		struct jbus **jbuses,
		int njbuses,
		int maxcount);

extern int jbus_read_write_dispatch(
		struct jbus *jbus,
		int toms);


/* verbs for the clients */
extern int jbus_call_ss(
		struct jbus *jbus,
		const char *method,
		const char *query,
		void (*onresp) (int, const char *, void *),
		void *data);

extern int jbus_call_js(
		struct jbus *jbus,
		const char *method,
		struct json_object *query,
		void (*onresp) (int, const char *, void *),
		void *data);

extern int jbus_call_sj(
		struct jbus *jbus,
		const char *method,
		const char *query,
		void (*onresp) (int, struct json_object *, void *),
		void *data);

extern int jbus_call_jj(
		struct jbus *jbus,
		const char *method,
		struct json_object *query,
		void (*onresp) (int, struct json_object *, void *),
		void *data);

extern char *jbus_call_ss_sync(
		struct jbus *jbus,
		const char *method,
		const char *query);

extern char *jbus_call_js_sync(
		struct jbus *jbus,
		const char *method,
		struct json_object *query);

extern struct json_object *jbus_call_sj_sync(
		struct jbus *jbus,
		const char *method,
		const char *query);

extern struct json_object *jbus_call_jj_sync(
		struct jbus *jbus,
		const char *method,
		struct json_object *query);

extern int jbus_on_signal_s(
		struct jbus *jbus,
		const char *name,
		void (*onsignal) (const char *, void *),
		void *data);

extern int jbus_on_signal_j(
		struct jbus *jbus,
		const char *name,
		void (*onsignal) (struct json_object *, void *),
		void *data);

/* verbs for servers */
extern int jbus_reply_s(
		struct jreq *jreq,
		const char *reply);

extern int jbus_reply_j(
		struct jreq *jreq,
		struct json_object *reply);

extern int jbus_reply_error_s(
		struct jreq *jreq,
		const char *reply);

extern int jbus_reply_error_j(
		struct jreq *jreq,
		struct json_object *reply);

extern int jbus_add_service_s(
		struct jbus *jbus,
		const char *method,
		void (*oncall) (struct jreq *, const char *, void *),
		void *data);

extern int jbus_add_service_j(
		struct jbus *jbus,
		const char *method,
		void (*oncall) (struct jreq *, struct json_object *, void *),
		void *data);

extern int jbus_start_serving(
		struct jbus *jbus);

extern int jbus_send_signal_s(
		struct jbus *jbus,
		const char *name,
		const char *content);

extern int jbus_send_signal_j(
		struct jbus *jbus,
		const char *name,
		struct json_object *content);

