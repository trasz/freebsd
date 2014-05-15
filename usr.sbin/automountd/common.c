/*-
 * Copyright (c) 2014 The FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed by Edward Tomasz Napierala under sponsorship
 * from the FreeBSD Foundation.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/linker.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libutil.h>

#include "autofs_ioctl.h"

#include "common.h"
#include "mntopts.h"

extern FILE *yyin;
extern char *yytext;
extern int lineno;
extern int yylex(void);

static void parse_master_yyin(struct node *root, const char *master);
static void parse_map_yyin(struct node *parent, const char *map);

char *
checked_strdup(const char *s)
{
	char *c;

	c = strdup(s);
	if (c == NULL)
		log_err(1, "strdup");
	return (c);
}

/*
 * Concatenate two strings, inserting separator between them, unless not needed.
 */
char *
separated_concat(const char *s1, const char *s2, char separator)
{
	char *result;
	int ret;

	if (s1[0] == '\0' || s2[0] == '\0' ||
	    s1[strlen(s1) - 1] == separator || s2[0] == separator) {
		ret = asprintf(&result, "%s%s", s1, s2);
	} else {
		ret = asprintf(&result, "%s%c%s", s1, separator, s2);
	}
	if (ret < 0)
		log_err(1, "asprintf");

	return (result);
}


struct node *
node_new_root(void)
{
	struct node *n;

	n = calloc(1, sizeof(*n));
	if (n == NULL)
		log_err(1, "calloc");
	// XXX
	n->n_key = checked_strdup("/");

	TAILQ_INIT(&n->n_children);

	return (n);
}

struct node *
node_new(struct node *parent, char *key, char *options, char *location,
    const char *config_file, int config_line)
{
	struct node *n;

	n = calloc(1, sizeof(*n));
	if (n == NULL)
		log_err(1, "calloc");

	TAILQ_INIT(&n->n_children);
	n->n_key = key;
	n->n_options = options;
	n->n_location = location;
	n->n_config_file = config_file;
	n->n_config_line = config_line;

	n->n_parent = parent;
	TAILQ_INSERT_TAIL(&parent->n_children, n, n_next);

	return (n);
}

static void
node_delete(struct node *n)
{
	struct node *n2, *tmp;

	TAILQ_FOREACH_SAFE(n2, &n->n_children, n_next, tmp)
		node_delete(n2);

	if (n->n_parent != NULL)
		TAILQ_REMOVE(&n->n_parent->n_children, n, n_next);

	free(n);
}

static bool
node_is_include(const struct node *n)
{
	if (n->n_key == NULL)
		return (false);
	if (n->n_key[0] != '+')
		return (false);
	return (true);
}

/*
 * Move (reparent) node 'n' to make it sibling of 'previous', placed
 * just after it.
 */
static void
node_move_after(struct node *n, struct node *previous)
{

	TAILQ_REMOVE(&n->n_parent->n_children, n, n_next);
	n->n_parent = previous->n_parent;
	TAILQ_INSERT_AFTER(&previous->n_parent->n_children, previous, n, n_next);
}

static void
node_expand_includes(struct node *root, bool is_master)
{
	struct node *n, *n2, *tmp, *tmp2, *tmproot;
	char *include = NULL;
	int error, ret;

	TAILQ_FOREACH_SAFE(n, &root->n_children, n_next, tmp) {
		if (node_is_include(n) == false)
			continue;

		/*
		 * "+1" to skip leading "+".
		 */
		ret = asprintf(&include, "%s %s",
		    AUTO_INCLUDE_PATH, n->n_key + 1);
		if (ret < 0)
			log_err(1, "asprintf");
		log_debugx("include \"%s\" maps to executable \"%s\"",
		    n->n_key, include);

		error = access(AUTO_INCLUDE_PATH, F_OK);
		if (error != 0) {
			log_errx(1, "directory services not configured; "
			    "%s does not exist", AUTO_INCLUDE_PATH);
		}

		yyin = popen(include, "r");
		if (yyin == NULL)
			log_err(1, "unable to execute \"%s\"", include);

		lineno = 0;

		tmproot = node_new_root();
		if (is_master)
			parse_master_yyin(tmproot, include);
		else
			parse_map_yyin(tmproot, include);

		error = pclose(yyin);
		yyin = NULL;
		if (error != 0)
			log_errx(1, "execution of \"%s\" failed", include);

		/*
		 * Entries to be included are now in tmproot.  We need to merge
		 * them with the rest, preserving their place and ordering.
		 */
		TAILQ_FOREACH_REVERSE_SAFE(n2,
		    &tmproot->n_children, nodehead, n_next, tmp2) {
			node_move_after(n2, n);
		}

		node_delete(n);
		node_delete(tmproot);
	}
}

static void
node_expand_defined(struct node *root)
{
	struct node *n;

	TAILQ_FOREACH(n, &root->n_children, n_next) {
		if (n->n_location != NULL)
			n->n_location = defined_expand(n->n_location);
		node_expand_defined(n);
	}
}

bool
node_is_direct_map(const struct node *n)
{

	for (;;) {
		assert(n->n_parent != NULL);
		if (n->n_parent->n_parent == NULL)
			break;
		n = n->n_parent;
	}

	assert(n->n_key != NULL);
	if (strcmp(n->n_key, "/-") != 0)
		return (false);

	return (true);
}

static void
node_expand_maps(struct node *n, bool indirect)
{
	struct node *n2, *tmp;

	TAILQ_FOREACH_SAFE(n2, &n->n_children, n_next, tmp) {
		if (node_is_direct_map(n2)) {
			if (indirect)
				continue;
		} else {
			if (indirect == false)
				continue;
		}

		/*
		 * This is the first-level map node; the one that contains
		 * the key and subnodes with mountpoints and actual map names.
		 */
		if (n2->n_location == NULL)
			continue;

		if (indirect) {
			log_debugx("map \"%s\" is an indirect map, parsing",
			    n2->n_location);
		} else {
			log_debugx("map \"%s\" is a direct map, parsing",
			    n2->n_location);
		}
		parse_map(n2, n2->n_location);
	}
}

static void
node_expand_direct_maps(struct node *n)
{

	node_expand_maps(n, false);
}

void
node_expand_indirect_maps(struct node *n)
{

	node_expand_maps(n, true);
}

static char *
node_mountpoint_x(const struct node *n, char *x)
{
	char *path;
	size_t len;

	if (n->n_parent == NULL)
		return (x);

	/*
	 * Return "/-" for direct maps only if we were asked for path
	 * to the "/-" node itself, not to any of its subnodes.
	 */
	if (n->n_parent->n_parent == NULL &&
	    strcmp(n->n_key, "/-") == 0 &&
	    x[0] != '\0') {
		return (x);
	}

	path = separated_concat(n->n_key, x, '/');
	free(x);

	/*
	 * Strip trailing slash.
	 */
	len = strlen(path);
	assert(path > 0);
	if (path[len - 1] == '/')
		path[len - 1] = '\0';

	return (node_mountpoint_x(n->n_parent, path));
}

char *
node_mountpoint(const struct node *n)
{

	return (node_mountpoint_x(n, checked_strdup("")));
}

static void
node_print_indent(const struct node *n, int indent)
{
	const struct node *n2, *first_child;
	char *n_mountpoint;

	n_mountpoint = node_mountpoint(n);
	first_child = TAILQ_FIRST(&n->n_children);

	/*
	 * Do not show both parent and child node if they have the same
	 * mountpoint; only show the child node.  This means the typical,
	 * "key location", map entries are shown in a single line;
	 * the "key mountpoint1 location2 mountpoint2 location2" entries
	 * take multiple lines.
	 */
	if (first_child == NULL || TAILQ_NEXT(first_child, n_next) != NULL ||
	    strcmp(n_mountpoint, node_mountpoint(first_child)) != 0) {
		printf("%*.s%s    %s    %s\t# %s map %s at %s:%d\n", indent, "",
		    n_mountpoint,
		    n->n_options != NULL ? n->n_options : "",
		    n->n_location != NULL ? n->n_location : "",
		    node_is_direct_map(n) ? "direct" : "indirect",
		    indent == 0 ? "referenced" : "defined",
		    n->n_config_file, n->n_config_line);
	}

	TAILQ_FOREACH(n2, &n->n_children, n_next)
		node_print_indent(n2, indent + 2);
}

void
node_print(const struct node *n)
{
	const struct node *n2;

	TAILQ_FOREACH(n2, &n->n_children, n_next)
		node_print_indent(n2, 0);
}

struct node *
node_find(struct node *root, const char *mountpoint)
{
	struct node *n, *n2;
	char *tmp;

	if (strcmp(root->n_key, "*") == 0)
		return (root);

	tmp = node_mountpoint(root);
	if (strcmp(tmp, mountpoint) == 0) {
		free(tmp);
		return (root);
	}
	free(tmp);

	TAILQ_FOREACH(n, &root->n_children, n_next) {
		n2 = node_find(n, mountpoint);
		if (n2 != NULL)
			return (n2);
	}

	return (NULL);
}

/*
 * Canonical form of a map entry looks like this:
 *
 * key [-options] [ [/mountpoint] [-options2] location ... ]
 *
 * We parse it in such a way that a map always has two levels - first
 * for key, and the second, for the mountpoint.
 */
static void
parse_map_yyin(struct node *parent, const char *map)
{
	char *key = NULL, *options = NULL, *mountpoint = NULL,
	    *options2 = NULL, *location = NULL;
	int ret;
	struct node *node;

	for (;;) {
		ret = yylex();
		if (ret == 0 || ret == NEWLINE) {
			if (key != NULL || options != NULL) {
				log_errx(1, "truncated entry in %s, line %d",
				    map, lineno);
			}
			if (ret == 0) {
				/*
				 * End of file.
				 */
				break;
			} else {
				key = options = NULL;
				continue;
			}
		}
		if (key == NULL) {
			key = checked_strdup(yytext);
			if (key[0] == '+') {
				node_new(parent, key, NULL, NULL, map, lineno);
				key = options = NULL;
				continue;
			}
			continue;
		} else if (yytext[0] == '-') {
			if (options != NULL) {
				log_errx(1, "duplicated options in %s, line %d",
				    map, lineno);
			}
			options = checked_strdup(yytext);
			continue;
		}

		//log_debugx("adding map node, %s", key);
		node = node_new(parent, key, options, NULL, map, lineno);
		key = options = NULL;

		for (;;) {
			if (yytext[0] == '/') {
				if (mountpoint != NULL) {
					log_errx(1, "duplicated mountpoint "
					    "in %s, line %d", map, lineno);
				}
				if (options2 != NULL || location != NULL) {
					log_errx(1, "mountpoint out of order "
					    "in %s, line %d", map, lineno);
				}
				mountpoint = checked_strdup(yytext);
				goto again;
			}

			if (yytext[0] == '-') {
				if (options2 != NULL) {
					log_errx(1, "duplicated options "
					    "in %s, line %d", map, lineno);
				}
				if (location != NULL) {
					log_errx(1, "options out of order "
					    "in %s, line %d", map, lineno);
				}
				options2 = checked_strdup(yytext);
				goto again;
			}

			if (location != NULL) {
				log_errx(1, "too many arguments "
				    "in %s, line %d", map, lineno);
			}
			location = checked_strdup(yytext);

			if (mountpoint == NULL)
				mountpoint = checked_strdup("/");
			if (options2 == NULL)
				options2 = checked_strdup("");

#if 0
			log_debugx("adding map node, %s %s %s",
			    mountpoint, options2, location);
#endif
			node_new(node, mountpoint, options2, location,
			    map, lineno);
			mountpoint = options2 = location = NULL;
again:
			ret = yylex();
			if (ret == 0 || ret == NEWLINE) {
				if (mountpoint != NULL || options2 != NULL ||
				    location != NULL) {
					log_errx(1, "truncated entry "
					    "in %s, line %d", map, lineno);
				}
				break;
			}
		}
	}
}

static bool
file_is_executable(const char *path)
{
	struct stat sb;
	int error;

	error = stat(path, &sb);
	if (error != 0)
		log_err(1, "cannot stat %s", path);
	if ((sb.st_mode & S_IXUSR) || (sb.st_mode & S_IXGRP) ||
	    (sb.st_mode & S_IXOTH))
		return (true);
	return (false);
}

void
parse_map(struct node *parent, const char *map)
{
	char *path = NULL;
	int error, ret;
	bool executable = false;

	assert(map != NULL);
	assert(map[0] != '\0');

	log_debugx("parsing map \"%s\"", map);

	if (map[0] == '-') {
		ret = asprintf(&path, "%s/special_%s",
		    AUTO_SPECIAL_PREFIX, map + 1);
		if (ret < 0)
			log_err(1, "asprintf");
		log_debugx("special map \"%s\" maps to executable \"%s\"",
		    map, path);
		executable = true;
	} else if (map[0] == '/') {
		path = checked_strdup(map);
	} else {
		ret = asprintf(&path, "%s/%s", AUTO_MAP_PREFIX, map);
		if (ret < 0)
			log_err(1, "asprintf");
		log_debugx("map \"%s\" maps to \"%s\"", map, path);

		/*
		 * See if the file exists.  If not, try to obtain the map
		 * from directory services.
		 */
		error = access(path, F_OK);
		if (error != 0) {
			log_debugx("map file \"%s\" does not exist; falling "
			    "back to directory services", path);

			error = access(AUTO_INCLUDE_PATH, F_OK);
			if (error != 0) {
				log_errx(1, "directory services not configured;"
				    " %s does not exist", AUTO_INCLUDE_PATH);
			}

			ret = asprintf(&path, "%s %s", AUTO_INCLUDE_PATH, map);
			if (ret < 0)
				log_err(1, "asprintf");
			log_debugx("map \"%s\" maps to executable \"%s\"",
			    map, path);
			executable = true;
		}
	}

	if (!executable) {
		executable = file_is_executable(path);
		if (executable)
			log_debugx("map \"%s\" is executable", map);
	}

	if (executable) {
		yyin = popen(path, "r");
		if (yyin == NULL)
			log_err(1, "unable to execute \"%s\"", path);
	} else {
		yyin = fopen(path, "r");
		if (yyin == NULL)
			log_err(1, "unable to open \"%s\"", path);
	}

	free(path);
	path = NULL;

	/*
	 * XXX: Here it's 1, below it's 0, and both work correctly; investigate.
	 */
	lineno = 1;

	parse_map_yyin(parent, map);

	if (executable) {
		error = pclose(yyin);
		if (error != 0) {
			log_errx(1, "execution of dynamic map \"%s\" failed",
			    map);
		}
	} else {
		fclose(yyin);
	}
	yyin = NULL;

	log_debugx("done parsing map \"%s\"", map);

	node_expand_includes(parent, false);
	node_expand_direct_maps(parent);
	node_expand_defined(parent);
}

static void
parse_master_yyin(struct node *root, const char *master)
{
	char *mountpoint = NULL, *map = NULL, *options = NULL;
	int ret;

	for (;;) {
		ret = yylex();
		if (ret == 0 || ret == NEWLINE) {
			if (mountpoint != NULL) {
				//log_debugx("adding map for %s", mountpoint);
				node_new(root, mountpoint, options, map,
				    master, lineno);
			}
			if (ret == 0) {
				break;
			} else {
				mountpoint = map = options = NULL;
				continue;
			}
		}
		if (mountpoint == NULL) {
			mountpoint = checked_strdup(yytext);
		} else if (map == NULL) {
			map = checked_strdup(yytext);
		} else if (options == NULL) {
			options = checked_strdup(yytext);
		} else {
			log_errx(1, "too many arguments in %s, line %d",
			    master, lineno);
		}
	}
}

void
parse_master(struct node *root, const char *master)
{

	log_debugx("parsing auto_master file at \"%s\"", master);

	yyin = fopen(master, "r");
	if (yyin == NULL)
		err(1, "unable to open %s", master);

	lineno = 0;

	parse_master_yyin(root, master);

	fclose(yyin);
	yyin = NULL;

	log_debugx("done parsing \"%s\"", master);

	node_expand_includes(root, true);
	node_expand_direct_maps(root);
	node_expand_defined(root);
}

int
main(int argc, char **argv)
{
	char *cmdname;

	if (argv[0] == NULL)
		log_errx(1, "NULL command name");

	cmdname = basename(argv[0]);

	if (strcmp(cmdname, "automount") == 0)
		return (main_automount(argc, argv));
	else if (strcmp(cmdname, "automountd") == 0)
		return (main_automountd(argc, argv));
	else if (strcmp(cmdname, "autounmountd") == 0)
		return (main_autounmountd(argc, argv));
	else
		log_errx(1, "binary name should be either \"automount\", "
		    "\"automountd\", or \"autounmountd\"");
}
