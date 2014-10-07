/*-
 * Copyright (c) 2011-2014 Juan Romero Pardines.
 * Copyright (c) 2014 Enno Boland.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/utsname.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#define _BSD_SOURCE	/* required by strlcpy with musl */
#include <string.h>
#undef _BSD_SOURCE
#include <strings.h>
#include <errno.h>
#include <stdarg.h>
#include <dirent.h>
#include <ctype.h>
#include <glob.h>
#include <libgen.h>

#include "xbps_api_impl.h"

#ifdef __clang__
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif

static int parse_file(struct xbps_handle *, const char *, const char *, bool, bool);

/**
 * @file lib/initend.c
 * @brief Initialization and finalization routines
 * @defgroup initend Initialization and finalization functions
 *
 * Use these functions to initialize some parameters before start
 * using libxbps and finalize usage to release resources at the end.
 */
static void
store_vpkg(struct xbps_handle *xhp, const char *path, size_t line, char *vpkg_s)
{
	/*
	 * Append virtual package overrides to our vpkgd dictionary:
	 *
	 * <key>vpkgver</key>
	 * <string>realpkgname</string>
	 */
	char *vpkg, *rpkg, *tc;
	size_t vpkglen;

	if (xhp->vpkgd == NULL)
		xhp->vpkgd = xbps_dictionary_create();

	/* real pkg after ':' */
	vpkg = vpkg_s;
	rpkg = strchr(vpkg_s, ':');
	if (rpkg == NULL || *rpkg == '\0') {
		xbps_dbg_printf(xhp, "%s: ignoring invalid "
		    "virtualpkg option at line %zu\n", path, line);
		return;
	}
	/* vpkg until ':' */
	tc = strchr(vpkg_s, ':');
	vpkglen = strlen(vpkg_s) - strlen(tc);
	vpkg[vpkglen] = '\0';

	/* skip ':' */
	rpkg++;
	xbps_dictionary_set_cstring(xhp->vpkgd, vpkg, rpkg);
	xbps_dbg_printf(xhp, "%s: added vpkg %s for %s\n", path, vpkg, rpkg);
}

static void
store_preserved_file(struct xbps_handle *xhp, const char *file)
{
	glob_t globbuf;
	char *p = NULL, *rfile = NULL;
	size_t len;
	int rv = 0;

	if (xhp->preserved_files == NULL) {
		xhp->preserved_files = xbps_array_create();
		assert(xhp->preserved_files);
	}

	rfile = xbps_xasprintf("%s%s", xhp->rootdir, file);

	rv = glob(rfile, 0, NULL, &globbuf);
	if (rv == GLOB_NOMATCH) {
		if (xbps_match_string_in_array(xhp->preserved_files, file))
			goto out;
		xbps_array_add_cstring(xhp->preserved_files, file);
		xbps_dbg_printf(xhp, "Added preserved file: %s\n", file);
		goto out;
	} else if (rv != 0) {
		goto out;
	}
	for (size_t i = 0; i < globbuf.gl_pathc; i++) {
		if (xbps_match_string_in_array(xhp->preserved_files, globbuf.gl_pathv[i]))
			continue;

		len = strlen(globbuf.gl_pathv[i]) - strlen(xhp->rootdir) + 1;
		p = malloc(len);
		assert(p);
		strlcpy(p, globbuf.gl_pathv[i] + strlen(xhp->rootdir), len);
		xbps_array_add_cstring(xhp->preserved_files, p);
		xbps_dbg_printf(xhp, "Added preserved file: %s (expanded from %s)\n", p, file);
		free(p);
	}
out:
	globfree(&globbuf);
	free(rfile);
}

static bool
store_repo(struct xbps_handle *xhp, const char *repo)
{
	if (xhp->repositories == NULL)
		xhp->repositories = xbps_array_create();

	if (xbps_match_string_in_array(xhp->repositories, repo))
		return false;

	xbps_array_add_cstring(xhp->repositories, repo);
	return true;
}

static bool
parse_option(char *buf, char **k, char **v)
{
	size_t klen;
	char *key, *value;
	const char *keys[] = {
		"rootdir",
		"cachedir",
		"syslog",
		"repository",
		"virtualpkg",
		"include",
		"preserve"
	};
	bool found = false;

	for (unsigned int i = 0; i < __arraycount(keys); i++) {
		key = __UNCONST(keys[i]);
		klen = strlen(key);
		if (strncmp(buf, key, klen) == 0) {
			found = true;
			break;
		}
	}
	/* non matching option */
	if (!found)
		return false;

	/* check if next char is the equal sign */
	if (buf[klen] != '=')
		return false;

	/* skip equal sign */
	value = buf + klen + 1;
	/* eat blanks */
	while (isblank((unsigned char)*value))
		value++;
	/* eat final newline */
	value[strlen(value)-1] = '\0';
	/* option processed successfully */
	*k = key;
	*v = value;

	return true;
}

static int
parse_files_glob(struct xbps_handle *xhp, const char *cwd, const char *path, bool nested, bool vpkgconf)
{
	glob_t globbuf;
	int rv = 0;

	glob(path, 0, NULL, &globbuf);
	for (size_t i = 0; i < globbuf.gl_pathc; i++) {
		if ((rv = parse_file(xhp, cwd, globbuf.gl_pathv[i], nested, vpkgconf)) != 0)
			break;
	}
	globfree(&globbuf);

	return rv;
}

static int
parse_file(struct xbps_handle *xhp, const char *cwd, const char *path, bool nested, bool vpkgconf)
{
	FILE *fp;
	char tmppath[XBPS_MAXPATH] = {0};
	size_t len, nlines = 0;
	ssize_t nread;
	char *cfcwd, *line = NULL;
	int rv = 0;

	if ((fp = fopen(path, "r")) == NULL) {
		rv = errno;
		xbps_dbg_printf(xhp, "cannot read configuration file %s: %s\n", path, strerror(rv));
		return rv;
	}

	if (!vpkgconf) {
		xbps_dbg_printf(xhp, "Parsing configuration file: %s\n", path);
	}

	while ((nread = getline(&line, &len, fp)) != -1) {
		char *p, *k, *v;

		nlines++;
		p = line;
		/* eat blanks */
		while (isblank((unsigned char)*p))
			p++;
		/* ignore comments or empty lines */
		if (*p == '#' || *p == '\n')
			continue;
		if (!parse_option(p, &k, &v)) {
			xbps_dbg_printf(xhp, "%s: ignoring invalid option at "
			    "line %zu\n", path, nlines);
			continue;
		}
		if (strcmp(k, "rootdir") == 0) {
			xbps_dbg_printf(xhp, "%s: rootdir set to %s\n",
			    path, v);
			snprintf(xhp->rootdir, sizeof(xhp->rootdir), "%s", v);
		} else if (strcmp(k, "cachedir") == 0) {
			xbps_dbg_printf(xhp, "%s: cachedir set to %s\n",
			    path, v);
			snprintf(xhp->cachedir, sizeof(xhp->cachedir), "%s", v);
		} else if (strcmp(k, "syslog") == 0) {
			if (strcasecmp(v, "true") == 0) {
				xhp->flags &= ~XBPS_FLAG_DISABLE_SYSLOG;
				xbps_dbg_printf(xhp, "%s: syslog enabled\n", path);
			} else {
				xhp->flags |= XBPS_FLAG_DISABLE_SYSLOG;
				xbps_dbg_printf(xhp, "%s: syslog disabled\n", path);
			}
		} else if (strcmp(k, "repository") == 0) {
			if (store_repo(xhp, v))
				xbps_dbg_printf(xhp, "%s: added repository %s\n", path, v);
		} else if (strcmp(k, "virtualpkg") == 0) {
			store_vpkg(xhp, path, nlines, v);
		} else if (strcmp(k, "preserve") == 0) {
			store_preserved_file(xhp, v);
		}
		/* Avoid double-nested parsing, only allow it once */
		if (nested)
			continue;

		if (strcmp(k, "include"))
			continue;

		/* cwd to the dir containing the config file */
		strlcpy(tmppath, path, sizeof(tmppath));
		cfcwd = dirname(tmppath);
		if (chdir(cfcwd) == -1) {
			rv = errno;
			xbps_dbg_printf(xhp, "cannot chdir to %s: %s\n", cfcwd, strerror(rv));
			return rv;
		}
		if ((rv = parse_files_glob(xhp, cwd, v, true, false)) != 0)
			break;

	}
	free(line);
	fclose(fp);

	return rv;
}

static int
parse_dir(struct xbps_handle *xhp, const char *cwd, const char *dir, const char *confdir, bool vpkg)
{
	struct dirent **namelist;
	char *ext, ldir[PATH_MAX], conf[PATH_MAX];
	int i, n, rv = 0;

	/*
	 * Read all configuration files stored in the system
	 * foo.d directory.
	 */
	snprintf(ldir, sizeof(ldir), "%s/%s",
		strcmp(xhp->rootdir, "/") ? xhp->rootdir : "", dir);
	xbps_dbg_printf(xhp, "Processing system directory: %s\n", ldir);

	if ((n = scandir(ldir, &namelist, 0, alphasort)) < 0)
		goto stage2;

	for (i = 0; i < n; i++) {
		if ((strcmp(namelist[i]->d_name, "..") == 0) ||
		    (strcmp(namelist[i]->d_name, ".") == 0)) {
			free(namelist[i]);
			continue;
		}
		/* only process .vpkg/.conf files, ignore something else */
		if ((ext = strrchr(namelist[i]->d_name, '.')) == NULL) {
			free(namelist[i]);
			continue;
		}
		if (strcmp(ext, ".conf") && strcmp(ext, ".vpkg")) {
			xbps_dbg_printf(xhp, "%s: ignoring %s\n", ldir, namelist[i]->d_name);
			free(namelist[i]);
			continue;
		}
		/* if the same file exists in configuration directory, ignore it */
		snprintf(conf, sizeof(conf), "%s/%s/%s", xhp->rootdir, confdir, namelist[i]->d_name);
		if (access(conf, R_OK) == 0) {
			xbps_dbg_printf(xhp, "%s: ignoring %s (exists in confdir)\n", ldir, namelist[i]->d_name);
			free(namelist[i]);
			continue;
		}
		/* parse conf file */
		snprintf(conf, sizeof(conf), "%s/%s", ldir, namelist[i]->d_name);
		if ((rv = parse_file(xhp, cwd, conf, false, vpkg)) != 0) {
			free(namelist[i]);
			break;
		}
	}
	free(namelist);
	if (rv != 0)
		return rv;

stage2:
	/*
	 * Read all configuration files stored in the configuration foo.d directory.
	 */
	snprintf(ldir, sizeof(ldir), "%s%s",
		strcmp(xhp->rootdir, "/") ? xhp->rootdir : "", confdir);
	xbps_dbg_printf(xhp, "Processing configuration directory: %s\n", ldir);

	if ((n = scandir(ldir, &namelist, 0, alphasort)) < 0)
		return 0;

	for (i = 0; i < n; i++) {
		if ((strcmp(namelist[i]->d_name, "..") == 0) ||
		    (strcmp(namelist[i]->d_name, ".") == 0)) {
			free(namelist[i]);
			continue;
		}
		/* only process .vpkg/.conf files, ignore something else */
		if ((ext = strrchr(namelist[i]->d_name, '.')) == NULL) {
			free(namelist[i]);
			continue;
		}
		if (strcmp(ext, ".conf") && strcmp(ext, ".vpkg")) {
			xbps_dbg_printf(xhp, "%s: ignoring %s\n", ldir, namelist[i]->d_name);
			free(namelist[i]);
			continue;
		}
		/* parse conf file */
		snprintf(conf, sizeof(conf), "%s/%s", ldir, namelist[i]->d_name);
		if ((rv = parse_file(xhp, cwd, conf, false, vpkg)) != 0) {
			free(namelist[i]);
			break;
		}
	}
	free(namelist);

	return rv;
}

int
xbps_init(struct xbps_handle *xhp)
{
	struct utsname un;
	char cwd[PATH_MAX-1], *buf;
	const char *repodir, *native_arch;
	int rv;

	assert(xhp != NULL);

	/* get cwd */
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return ENOTSUP;

	/* set conffile */
	if (xhp->conffile[0] == '\0') {
		snprintf(xhp->conffile, sizeof(xhp->conffile), XBPS_CONF_DEF);
	} else {
		buf = strdup(xhp->conffile);
		snprintf(xhp->conffile, sizeof(xhp->conffile), "%s/%s", cwd, buf);
		free(buf);
	}
	/* Set rootdir */
	if (xhp->rootdir[0] == '\0') {
		xhp->rootdir[0] = '/';
		xhp->rootdir[1] = '\0';
	} else if (xhp->rootdir[0] != '/') {
		buf = strdup(xhp->rootdir);
		snprintf(xhp->rootdir, sizeof(xhp->rootdir), "%s/%s", cwd, buf);
		free(buf);
	}
	/* parse configuration file */
	xbps_dbg_printf(xhp, "%s\n", XBPS_RELVER);
	if ((rv = parse_file(xhp, cwd, xhp->conffile, false, false)) != 0) {
		xbps_dbg_printf(xhp, "Using built-in defaults\n");
	}
	/* Set cachedir */
	if (xhp->cachedir[0] == '\0') {
		snprintf(xhp->cachedir, sizeof(xhp->cachedir),
		    "%s/%s", strcmp(xhp->rootdir, "/") ? xhp->rootdir : "",
		    XBPS_CACHE_PATH);
	} else if (xhp->cachedir[0] != '/') {
		/* relative path */
		buf = strdup(xhp->cachedir);
		snprintf(xhp->cachedir, sizeof(xhp->cachedir),
		    "%s/%s", strcmp(xhp->rootdir, "/") ? xhp->rootdir : "", buf);
		free(buf);
	}
	/* Set metadir */
	if (xhp->metadir[0] == '\0') {
		snprintf(xhp->metadir, sizeof(xhp->metadir),
		    "%s/%s", strcmp(xhp->rootdir, "/") ? xhp->rootdir : "",
		    XBPS_META_PATH);
	} else if (xhp->metadir[0] != '/') {
		/* relative path */
		buf = strdup(xhp->metadir);
		snprintf(xhp->metadir, sizeof(xhp->metadir),
		    "%s/%s", strcmp(xhp->rootdir, "/") ? xhp->rootdir : "", buf);
		free(buf);
	}
	/* process virtualpkg.d dirs */
	if ((rv = parse_dir(xhp, cwd, XBPS_SYS_VPKG_PATH, XBPS_VPKG_PATH, true)) != 0)
		return rv;

	/* process repo.d dirs */
	if ((rv = parse_dir(xhp, cwd, XBPS_SYS_REPOD_PATH, XBPS_REPOD_PATH, false)) != 0)
		return rv;

	/* process preserve.d dirs */
	if ((rv = parse_dir(xhp, cwd, XBPS_SYS_PRESERVED_PATH, XBPS_PRESERVED_PATH, false)) != 0)
		return rv;

	xhp->target_arch = getenv("XBPS_TARGET_ARCH");
	if ((native_arch = getenv("XBPS_ARCH")) != NULL) {
		strlcpy(xhp->native_arch, native_arch, sizeof(xhp->native_arch));
	} else {
		uname(&un);
		strlcpy(xhp->native_arch, un.machine, sizeof(xhp->native_arch));
	}
	assert(xhp->native_arch);

	xbps_fetch_set_cache_connection(XBPS_FETCH_CACHECONN, XBPS_FETCH_CACHECONN_HOST);

	xbps_dbg_printf(xhp, "rootdir=%s\n", xhp->rootdir);
	xbps_dbg_printf(xhp, "metadir=%s\n", xhp->metadir);
	xbps_dbg_printf(xhp, "cachedir=%s\n", xhp->cachedir);
	xbps_dbg_printf(xhp, "syslog=%s\n", xhp->flags & XBPS_FLAG_DISABLE_SYSLOG ? "false" : "true");
	xbps_dbg_printf(xhp, "Architecture: %s\n", xhp->native_arch);
	xbps_dbg_printf(xhp, "Target Architecture: %s\n", xhp->target_arch);

	if (xhp->flags & XBPS_FLAG_DEBUG) {
		for (unsigned int i = 0; i < xbps_array_count(xhp->repositories); i++) {
			xbps_array_get_cstring_nocopy(xhp->repositories, i, &repodir);
			xbps_dbg_printf(xhp, "Repository[%u]=%s\n", i, repodir);
		}
	}
	/* Going back to old working directory */
	if (chdir(cwd) == -1)
		xbps_dbg_printf(xhp, "%s: cannot chdir to %s: %s\n", __func__, cwd, strerror(errno));

	return 0;
}

void
xbps_end(struct xbps_handle *xhp)
{
	assert(xhp);

	xbps_pkgdb_release(xhp);
	xbps_fetch_unset_cache_connection();
}

static void
common_printf(FILE *f, const char *msg, const char *fmt, va_list ap)
{
	if (msg != NULL)
		fprintf(f, "%s", msg);

	vfprintf(f, fmt, ap);
}

void
xbps_dbg_printf_append(struct xbps_handle *xhp, const char *fmt, ...)
{
	va_list ap;

	if ((xhp->flags & XBPS_FLAG_DEBUG) == 0)
		return;

	va_start(ap, fmt);
	common_printf(stderr, NULL, fmt, ap);
	va_end(ap);
}

void
xbps_dbg_printf(struct xbps_handle *xhp, const char *fmt, ...)
{
	va_list ap;

	if ((xhp->flags & XBPS_FLAG_DEBUG) == 0)
		return;

	va_start(ap, fmt);
	common_printf(stderr, "[DEBUG] ", fmt, ap);
	va_end(ap);
}

void
xbps_error_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	common_printf(stderr, "ERROR: ", fmt, ap);
	va_end(ap);
}

void
xbps_warn_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	common_printf(stderr, "WARNING: ", fmt, ap);
	va_end(ap);
}
