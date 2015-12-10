/*
 Copyright 2015 IoT.bzh

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

#define _BSD_SOURCE /* see readdir */

#include <limits.h>
#include <zip.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <dirent.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "wgtpkg.h"


#if !defined(MODE_OF_FILE_CREATION)
#define MODE_OF_FILE_CREATION 0640
#endif
#if !defined(MODE_OF_DIRECTORY_CREATION)
#define MODE_OF_DIRECTORY_CREATION 0750
#endif

static int is_valid_filename(const char *filename)
{
	int lastsp = 0;
	int index = 0;
	unsigned char c;

	c = (unsigned char)filename[index];
	while (c) {
		if ((c < 0x1f)
		 || ((lastsp = (c == 0x20)) && index == 0)
		 || c == 0x7f || c == 0x3c || c == 0x3e
		 || c == 0x3a || c == 0x22 
		 || c == 0x5c || c == 0x7c || c == 0x3f
		 || c == 0x2a || c == 0x5e || c == 0x60
		 || c == 0x7b || c == 0x7d || c == 0x21)
			return 0;
		c = (unsigned char)filename[++index];
	}
	return !lastsp;
}

static int create_directory(char *file, int mode)
{
	int rc;
	char *last = strrchr(file, '/');
	if (last != NULL)
		*last = 0;
	rc = mkdir(file, mode);
	if (rc) {
		if (errno == EEXIST)
			rc = 0;
		else if (errno == ENOENT) {
			rc = create_directory(file, mode);
			if (!rc)
				rc = mkdir(file, mode);
		}
	}
	if (rc)
		syslog(LOG_ERR, "can't create directory %s", file);
	if (last != NULL)
		*last = '/';
	return rc;
}

static int create_file(char *file, int fmode, int dmode)
{
	int fd = creat(file, fmode);
	if (fd < 0 && errno == ENOENT) {
		if (!create_directory(file, dmode))
			fd = creat(file, fmode);
	}
	if (fd < 0)
		syslog(LOG_ERR, "can't create file %s", file);
	return fd;
}

/* read (extract) 'zipfile' in current directory */
int zread(const char *zipfile, unsigned long long maxsize)
{
	struct filedesc *fdesc;
	int err, fd, len;
	struct zip *zip;
	zip_int64_t z64;
	unsigned int count, index;
	struct zip_file *zfile;
	struct zip_stat zstat;
	char buffer[32768];
	ssize_t sizr, sizw;
	size_t esize;

	/* open the zip file */
	zip = zip_open(zipfile, ZIP_CHECKCONS, &err);
	if (!zip) {
		syslog(LOG_ERR, "Can't connect to file %s", zipfile);
		return -1;
	}

	z64 = zip_get_num_entries(zip, 0);
	if (z64 < 0 || z64 > UINT_MAX) {
		syslog(LOG_ERR, "too many entries in %s", zipfile);
		goto error;
	}
	count = (unsigned int)z64;

	/* records the files */
	file_reset();
	esize = 0;
	for (index = 0 ; index < count ; index++) {
		err = zip_stat_index(zip, index, ZIP_FL_ENC_GUESS, &zstat);
		/* check the file name */
		if (!is_valid_filename(zstat.name)) {
			syslog(LOG_ERR, "invalid entry %s found in %s", zstat.name, zipfile);
			goto error;
		}
		if (zstat.name[0] == '/') {
			syslog(LOG_ERR, "absolute entry %s found in %s", zstat.name, zipfile);
			goto error;
		}
		len = strlen(zstat.name);
		if (len == 0) {
			syslog(LOG_ERR, "empty entry found in %s", zipfile);
			goto error;
		}
		if (zstat.size == 0) {
			/* directory name */
			if (zstat.name[len - 1] != '/') {
				syslog(LOG_ERR, "bad directory name %s in %s", zstat.name, zipfile);
				goto error;
			}
			/* record */
			fdesc = file_add_directory(zstat.name);
		} else {
			/* directory name */
			if (zstat.name[len - 1] == '/') {
				syslog(LOG_ERR, "bad file name %s in %s", zstat.name, zipfile);
				goto error;
			}
			/* get the size */
			esize += zstat.size;
			/* record */
			fdesc = file_add_file(zstat.name);
		}
		if (!fdesc)
			goto error;
		fdesc->zindex = index;
	}

	/* check the size */
	if (maxsize && esize > maxsize) {
		syslog(LOG_ERR, "extracted size %zu greater than allowed size %llu", esize, maxsize);
		goto error;
	}

	/* unpack the recorded files */
	assert(count == file_count());
	for (index = 0 ; index < count ; index++) {
		fdesc = file_of_index(index);
		assert(fdesc != NULL);
		err = zip_stat_index(zip, fdesc->zindex, ZIP_FL_ENC_GUESS, &zstat);
		assert(zstat.name[0] != '/');
		if (zstat.size == 0) {
			/* directory name */
			err = create_directory((char*)zstat.name, MODE_OF_DIRECTORY_CREATION);
			if (err && errno != EEXIST)
				goto error;
		} else {
			/* file name */
			zfile = zip_fopen_index(zip, fdesc->zindex, 0);
			if (!zfile) {
				syslog(LOG_ERR, "Can't open %s in %s", zstat.name, zipfile);
				goto error;
			}
			fd = create_file((char*)zstat.name, MODE_OF_FILE_CREATION, MODE_OF_DIRECTORY_CREATION);
			if (fd < 0)
				goto errorz;
			/* extract */
			z64 = zstat.size;
			while (z64) {
				sizr = zip_fread(zfile, buffer, sizeof buffer);
				if (sizr < 0) {
					syslog(LOG_ERR, "error while reading %s in %s", zstat.name, zipfile);
					goto errorzf;
				}
				sizw = write(fd, buffer, sizr);
				if (sizw < 0) {
					syslog(LOG_ERR, "error while writing %s", zstat.name);
					goto errorzf;
				}
				z64 -= sizw;
			}
			close(fd);
			zip_fclose(zfile);
		}
	}

	zip_close(zip);
	return 0;

errorzf:
	close(fd);
errorz:
	zip_fclose(zfile);
error:
	zip_close(zip);
	return -1;
}

struct zws {
	struct zip *zip;
	char name[PATH_MAX];
	char buffer[32768];
};

static int zwr(struct zws *zws, int offset)
{
	int len, err;
	DIR *dir;
	struct dirent *ent;
	zip_int64_t z64;
	struct zip_source *zsrc;

	if (offset == 0)
		dir = opendir(".");
	else {
		dir = opendir(zws->name);
		zws->name[offset++] = '/';
	}
	if (!dir) {
		syslog(LOG_ERR, "opendir %.*s failed in zwr", offset, zws->name);
		return -1;
	}

	ent = readdir(dir);
	while (ent != NULL) {
		len = strlen(ent->d_name);
		if (ent->d_name[0] == '.' && (len == 1 || 
			(ent->d_name[1] == '.' && len == 2)))
			;
		else if (offset + len >= sizeof(zws->name)) {
			syslog(LOG_ERR, "name too long in zwr");
			errno = ENAMETOOLONG;
			goto error;
		} else {
			memcpy(zws->name + offset, ent->d_name, 1+len);
			if (!is_valid_filename(ent->d_name)) {
				syslog(LOG_ERR, "invalid name %s", zws->name);
				goto error;
			}
			switch (ent->d_type) {
			case DT_DIR:
				z64 = zip_dir_add(zws->zip, zws->name, ZIP_FL_ENC_UTF_8);
				if (z64 < 0) {
					syslog(LOG_ERR, "zip_dir_add of %s failed", zws->name);
					goto error;
				}
				err = zwr(zws, offset + len);
				if (err)
					goto error;
				break;
			case DT_REG:
				zsrc = zip_source_file(zws->zip, zws->name, 0, 0);
				if (zsrc == NULL) {
					syslog(LOG_ERR, "zip_source_file of %s failed", zws->name);
					goto error;
				}
				z64 = zip_file_add(zws->zip, zws->name, zsrc, ZIP_FL_ENC_UTF_8);
				if (z64 < 0) {
					syslog(LOG_ERR, "zip_file_add of %s failed", zws->name);
					zip_source_free(zsrc);
					goto error;
				}
				break;
			default:
				break;
			}
		}
		ent = readdir(dir);
	}

	closedir(dir);
	return 0;
error:
	closedir(dir);
	return -1;
}

/* write (pack) content of the current directory in 'zipfile' */
int zwrite(const char *zipfile)
{
	int err;
	struct zws zws;

	zws.zip = zip_open(zipfile, ZIP_CREATE|ZIP_TRUNCATE, &err);
	if (!zws.zip) {
		syslog(LOG_ERR, "Can't open %s for write", zipfile);
		return -1;
	}

	err = zwr(&zws, 0);
	zip_close(zws.zip);
	return err;
}


#if defined(TEST_READ)
int main(int ac, char **av)
{
	for(av++ ; *av ; av++)
		zread(*av);
	return 0;
}
#endif

#if defined(TEST_WRITE)
int main(int ac, char **av)
{
	for(av++ ; *av ; av++)
		zwrite(*av);
	return 0;
}
#endif
