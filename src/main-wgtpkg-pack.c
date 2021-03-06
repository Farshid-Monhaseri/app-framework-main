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
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "verbose.h"
#include "wgtpkg-files.h"
#include "wgtpkg-workdir.h"
#include "wgtpkg-zip.h"
#include "wgtpkg-digsig.h"

const char appname[] = "wgtpkg-pack";

static void version()
{
	printf(
		"\n"
		"  %s  version="AFM_VERSION"\n"
		"\n"
		"  Copyright (C) 2015-2020 \"IoT.bzh\"\n"
		"  AFB comes with ABSOLUTELY NO WARRANTY.\n"
		"  Licence Apache 2\n"
		"\n",
		appname
	);
}

static void usage()
{
	printf(
		"usage: %s [-f] [-o wgtfile] directory\n"
		"\n"
		"   -o wgtfile       the output widget file\n"
		"   -f               force overwriting\n"
		"   -N               no auto-sign"
		"   -q               quiet\n"
		"   -S               auto-sign"
		"   -v               verbose\n"
		"   -V               version\n"
		"\n",
		appname
	);
}

static struct option options[] = {
	{ "output",      required_argument, NULL, 'o' },
	{ "force",       no_argument,       NULL, 'f' },
	{ "help",        no_argument,       NULL, 'h' },
	{ "no-auto-sign",no_argument,       NULL, 'N' },
	{ "quiet",       no_argument,       NULL, 'q' },
	{ "auto-sign",   no_argument,       NULL, 'S' },
	{ "verbose",     no_argument,       NULL, 'v' },
	{ "version",     no_argument,       NULL, 'V' },
	{ NULL, 0, NULL, 0 }
};

/* install the widgets of the list */
int main(int ac, char **av)
{
	int i, force, autosign;
	char *wgtfile, *directory, *x;
	struct stat s;

	LOGUSER(appname);

	autosign = 1;
	force = 0;
	wgtfile = directory = NULL;
	for (;;) {
		i = getopt_long(ac, av, "qvVhfo:", options, NULL);
		if (i < 0)
			break;
		switch (i) {
		case 'o':
			wgtfile = optarg;
			break;
		case 'q':
			if (verbosity)
				verbosity--;
			break;
		case 'v':
			verbosity++;
			break;
		case 'f':
			force = 1;
			break;
		case 'h':
			usage();
			return 0;
		case 'N':
			autosign = 0;
			break;
		case 'V':
			version();
			return 0;
		case 'S':
			autosign = 1;
			break;
		case ':':
			ERROR("missing argument");
			return 1;
		default:
			ERROR("unrecognized option");
			return 1;
		}
	}

	/* remaining arguments and final checks */
	if (optind >= ac) {
		ERROR("no directory set");
		return 1;
	}
	directory = av[optind++];
	if (optind < ac) {
		ERROR("extra parameters found");
		return 1;
	}

	/* set default values */
	if (wgtfile == NULL && 0 > asprintf(&wgtfile, "%s.wgt", directory)) {
		ERROR("asprintf failed");
		return 1;
	}

	/* check values */
	if (stat(directory, &s)) {
		ERROR("can't find directory %s", directory);
		return 1;
	}
	if (!S_ISDIR(s.st_mode)) {
		ERROR("%s isn't a directory", directory);
		return 1;
	}
	if (access(wgtfile, F_OK) == 0 && force == 0) {
		ERROR("can't overwrite existing %s", wgtfile);
		return 1;
	}

	NOTICE("-- PACKING widget %s from directory %s", wgtfile, directory);

	/* creates an existing widget (for realpath it must exist) */
	i = open(wgtfile, O_WRONLY|O_CREAT|O_NOCTTY|O_NONBLOCK, 0644);
	if (i < 0) {
		ERROR("can't write widget %s", wgtfile);
		return 1;
	}
	close(i);

	/* compute absolutes paths */
	x = realpath(wgtfile, NULL);
	if (x == NULL) {
		ERROR("realpath failed for %s", wgtfile);
		return 1;
	}
	wgtfile = x;

	/* set and enter the workdir */
	if (chdir(directory)) {
		ERROR("failed to enter directory %s", directory);
		return 1;
	}
	if (set_workdir(".", 0))
		return 1;

	if (fill_files())
		return 1;

	if (autosign && create_auto_digsig() < 0)
		return 1;

	return !!zwrite(wgtfile);
}


