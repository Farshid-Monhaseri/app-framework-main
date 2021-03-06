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

struct wgt;

/* creation and reference */
extern struct wgt *wgt_create();
extern void wgt_addref(struct wgt *wgt);
extern void wgt_unref(struct wgt *wgt);

/* connection and disconnection */
extern struct wgt *wgt_createat(int dirfd, const char *pathname);
extern int wgt_connect(struct wgt *wgt, const char *pathname);
extern int wgt_connectat(struct wgt *wgt, int dirfd, const char *pathname);
extern void wgt_disconnect(struct wgt *wgt);
extern int wgt_is_connected(struct wgt *wgt);

/* management of locales */
extern void wgt_locales_reset(struct wgt *wgt);
extern int wgt_locales_add(struct wgt *wgt, const char *locstr);
extern unsigned int wgt_locales_score(struct wgt *wgt, const char *lang);

/* direct access to files */
extern int wgt_has(struct wgt *wgt, const char *filename);
extern int wgt_open_read(struct wgt *wgt, const char *filename);

/* localised access to files */
extern char *wgt_locales_locate(struct wgt *wgt, const char *filename);
extern int wgt_locales_open_read(struct wgt *wgt, const char *filename);

