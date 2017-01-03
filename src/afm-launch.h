/*
 Copyright 2015, 2016, 2017 IoT.bzh

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

/*
 * Structure describing what is to be launched
 */
struct afm_launch_desc {
	const char *path;          /* to the widget directory */
	const char *appid;         /* application identifier */
	const char *content;       /* content to launch */
	const char *type;          /* type to launch */
	const char *name;          /* name of the application */
	const char *home;          /* home directory of the applications */
	const char **bindings;     /* bindings for the application */
	int width;                 /* requested width */
	int height;                /* requested height */
	enum afm_launch_mode mode; /* launch mode */
};

int afm_launch_initialize();

int afm_launch(struct afm_launch_desc *desc, pid_t children[2], char **uri);

