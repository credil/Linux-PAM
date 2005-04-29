/*
 * Copyright 2001-2003 Red Hat, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * the GNU Public License, in which case the provisions of the GPL are
 * required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/* "$Id$" */

#include "../../_pam_aconf.h"
#include <sys/types.h>
#include <sys/fsuid.h>
#include <sys/wait.h>
#include <errno.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <netdb.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <security/pam_modules.h>
#include <security/_pam_macros.h>
#include <security/_pam_modutil.h>

#define DATANAME "pam_xauth_cookie_file"
#define XAUTHBIN "/usr/X11R6/bin/xauth"
#define XAUTHENV "XAUTHORITY"
#define HOMEENV  "HOME"
#define XAUTHDEF ".Xauthority"
#define XAUTHTMP ".xauthXXXXXX"

/* Run a given command (with a NULL-terminated argument list), feeding it the
 * given input on stdin, and storing any output it generates. */
static int
run_coprocess(const char *input, char **output,
	      uid_t uid, gid_t gid, const char *command, ...)
{
	int ipipe[2], opipe[2], i;
	char buf[LINE_MAX];
	pid_t child;
	char *buffer = NULL;
	size_t buffer_size = 0;
	va_list ap;

	*output = NULL;

	/* Create stdio pipery. */
	if (pipe(ipipe) == -1) {
		return -1;
	}
	if (pipe(opipe) == -1) {
		close(ipipe[0]);
		close(ipipe[1]);
		return -1;
	}

	/* Fork off a child. */
	child = fork();
	if (child == -1) {
		close(ipipe[0]);
		close(ipipe[1]);
		close(opipe[0]);
		close(opipe[1]);
		return -1;
	}

	if (child == 0) {
		/* We're the child. */
		char *args[10];
		const char *tmp;
		/* Drop privileges. */
		setgid(gid);
		setgroups(0, NULL);
		setuid(uid);
		/* Initialize the argument list. */
		memset(args, 0, sizeof(args));
		/* Set the pipe descriptors up as stdin and stdout, and close
		 * everything else, including the original values for the
		 * descriptors. */
		dup2(ipipe[0], STDIN_FILENO);
		dup2(opipe[1], STDOUT_FILENO);
		for (i = 0; i < sysconf(_SC_OPEN_MAX); i++) {
			if ((i != STDIN_FILENO) && (i != STDOUT_FILENO)) {
				close(i);
			}
		}
		/* Convert the varargs list into a regular array of strings. */
		va_start(ap, command);
		args[0] = strdup(command);
		for (i = 1; i < ((sizeof(args) / sizeof(args[0])) - 1); i++) {
			tmp = va_arg(ap, const char*);
			if (tmp == NULL) {
				break;
			}
			args[i] = strdup(tmp);
		}
		/* Run the command. */
		execvp(command, args);
		/* Never reached. */
		exit(1);
	}

	/* We're the parent, so close the other ends of the pipes. */
	close(ipipe[0]);
	close(opipe[1]);
	/* Send input to the process (if we have any), then send an EOF. */
	if (input) {
		(void)_pammodutil_write(ipipe[1], input, strlen(input));
	}
	close(ipipe[1]);

	/* Read data output until we run out of stuff to read. */
	i = _pammodutil_read(opipe[0], buf, sizeof(buf));
	while ((i != 0) && (i != -1)) {
		char *tmp;
		/* Resize the buffer to hold the data. */
		tmp = realloc(buffer, buffer_size + i + 1);
		if (tmp == NULL) {
			/* Uh-oh, bail. */
			if (buffer != NULL) {
				free(buffer);
			}
			close(opipe[0]);
			waitpid(child, NULL, 0);
			return -1;
		}
		/* Save the new buffer location, copy the newly-read data into
		 * the buffer, and make sure the result will be
		 * nul-terminated. */
		buffer = tmp;
		memcpy(buffer + buffer_size, buf, i);
		buffer[buffer_size + i] = '\0';
		buffer_size += i;
		/* Try to read again. */
		i = _pammodutil_read(opipe[0], buf, sizeof(buf));
	}
	/* No more data.  Clean up and return data. */
	close(opipe[0]);
	*output = buffer;
	waitpid(child, NULL, 0);
	return 0;
}

/* Free a data item. */
static void
cleanup(pam_handle_t *pamh, void *data, int err)
{
	free(data);
}

/* Check if we want to allow export to the other user, or import from the
 * other user. */
static int
check_acl(pam_handle_t *pamh,
	  const char *sense, const char *this_user, const char *other_user,
	  int noent_code, int debug)
{
	char path[PATH_MAX];
	struct passwd *pwd;
	FILE *fp;
	int i;
	uid_t euid;
	/* Check this user's <sense> file. */
	pwd = _pammodutil_getpwnam(pamh, this_user);
	if (pwd == NULL) {
		syslog(LOG_ERR, "pam_xauth: error determining "
		       "home directory for '%s'", this_user);
		return PAM_SESSION_ERR;
	}
	/* Figure out what that file is really named. */
	i = snprintf(path, sizeof(path), "%s/.xauth/%s", pwd->pw_dir, sense);
	if ((i >= sizeof(path)) || (i < 0)) {
		syslog(LOG_ERR, "pam_xauth: name of user's home directory "
		       "is too long");
		return PAM_SESSION_ERR;
	}
	euid = geteuid();
	setfsuid(pwd->pw_uid);
	fp = fopen(path, "r");
	setfsuid(euid);
	if (fp != NULL) {
		char buf[LINE_MAX], *tmp;
		/* Scan the file for a list of specs of users to "trust". */
		while (fgets(buf, sizeof(buf), fp) != NULL) {
			tmp = memchr(buf, '\r', sizeof(buf));
			if (tmp != NULL) {
				*tmp = '\0';
			}
			tmp = memchr(buf, '\n', sizeof(buf));
			if (tmp != NULL) {
				*tmp = '\0';
			}
			if (fnmatch(buf, other_user, 0) == 0) {
				if (debug) {
					syslog(LOG_DEBUG, "pam_xauth: %s %s "
					       "allowed by %s",
					       other_user, sense, path);
				}
				fclose(fp);
				return PAM_SUCCESS;
			}
		}
		/* If there's no match in the file, we fail. */
		if (debug) {
			syslog(LOG_DEBUG, "pam_xauth: %s not listed in %s",
			       other_user, path);
		}
		fclose(fp);
		return PAM_PERM_DENIED;
	} else {
		/* Default to okay if the file doesn't exist. */
		switch (errno) {
		case ENOENT:
			if (noent_code == PAM_SUCCESS) {
				if (debug) {
					syslog(LOG_DEBUG, "%s does not exist, "
					       "ignoring", path);
				}
			} else {
				if (debug) {
					syslog(LOG_DEBUG, "%s does not exist, "
					       "failing", path);
				}
			}
			return noent_code;
		default:
			if (debug) {
				syslog(LOG_ERR, "%s opening %s",
				       strerror(errno), path);
			}
			return PAM_PERM_DENIED;
		}
	}
}

int
pam_sm_open_session(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
	char xauthpath[] = XAUTHBIN;
	char *cookiefile = NULL, *xauthority = NULL,
	     *cookie = NULL, *display = NULL, *tmp = NULL;
	const char *user, *xauth = xauthpath;
	struct passwd *tpwd, *rpwd;
	int fd, i, debug = 0;
	uid_t systemuser = 499, targetuser = 0, euid;

	/* Parse arguments.  We don't understand many, so no sense in breaking
	 * this into a separate function. */
	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "debug") == 0) {
			debug = 1;
			continue;
		}
		if (strncmp(argv[i], "xauthpath=", 10) == 0) {
			xauth = argv[i] + 10;
			continue;
		}
		if (strncmp(argv[i], "targetuser=", 11) == 0) {
			long l = strtol(argv[i] + 11, &tmp, 10);
			if ((strlen(argv[i] + 11) > 0) && (*tmp == '\0')) {
				targetuser = l;
			} else {
				syslog(LOG_WARNING, "pam_xauth: invalid value "
				       "for targetuser (`%s')", argv[i] + 11);
			}
			continue;
		}
		if (strncmp(argv[i], "systemuser=", 11) == 0) {
			long l = strtol(argv[i] + 11, &tmp, 10);
			if ((strlen(argv[i] + 11) > 0) && (*tmp == '\0')) {
				systemuser = l;
			} else {
				syslog(LOG_WARNING, "pam_xauth: invalid value "
				       "for systemuser (`%s')", argv[i] + 11);
			}
			continue;
		}
		syslog(LOG_WARNING, "pam_xauth: unrecognized option `%s'",
		       argv[i]);
	}

	/* If DISPLAY isn't set, we don't really care, now do we? */
	if ((display = getenv("DISPLAY")) == NULL) {
		if (debug) {
			syslog(LOG_DEBUG, "pam_xauth: user has no DISPLAY,"
			       " doing nothing");
		}
		return PAM_SUCCESS;
	}

	/* Read the target user's name. */
	if (pam_get_user(pamh, &user, NULL) != PAM_SUCCESS) {
		syslog(LOG_ERR, "pam_xauth: error determining target "
		       "user's name");
		return PAM_SESSION_ERR;
	}
	rpwd = _pammodutil_getpwuid(pamh, getuid());
	if (rpwd == NULL) {
		syslog(LOG_ERR, "pam_xauth: error determining invoking "
		       "user's name");
		return PAM_SESSION_ERR;
	}

	/* Get the target user's UID and primary GID, which we'll need to set
	 * on the xauthority file we create later on. */
	tpwd = _pammodutil_getpwnam(pamh, user);
	if (tpwd == NULL) {
		syslog(LOG_ERR, "pam_xauth: error determining target "
		       "user's UID");
		return PAM_SESSION_ERR;
	}

	if (debug) {
		syslog(LOG_DEBUG, "pam_xauth: requesting user %lu/%lu, "
		       "target user %lu/%lu",
		       (unsigned long) rpwd->pw_uid,
		       (unsigned long) rpwd->pw_gid,
		       (unsigned long) tpwd->pw_uid,
		       (unsigned long) tpwd->pw_gid);
	}

	/* If the UID is a system account (and not the superuser), forget
	 * about forwarding keys. */
	if ((tpwd->pw_uid != 0) &&
	    (tpwd->pw_uid != targetuser) &&
	    (tpwd->pw_uid <= systemuser)) {
		if (debug) {
			syslog(LOG_DEBUG, "pam_xauth: not forwarding cookies "
			       "to user ID %ld", (long) tpwd->pw_uid);
		}
		return PAM_SESSION_ERR;
	}

	/* Check that both users are amenable to this.  By default, this
	 * boils down to this policy:
	 * export(ruser=root): only if <user> is listed in .xauth/export
	 * export(ruser=*) if <user> is listed in .xauth/export, or
	 *                 if .xauth/export does not exist
	 * import(user=*): if <ruser> is listed in .xauth/import, or
	 *                 if .xauth/import does not exist */
	i = (getuid() != 0) ? PAM_SUCCESS : PAM_PERM_DENIED;
	i = check_acl(pamh, "export", rpwd->pw_name, user, i, debug);
	if (i != PAM_SUCCESS) {
		return PAM_SESSION_ERR;
	}
	i = PAM_SUCCESS;
	i = check_acl(pamh, "import", user, rpwd->pw_name, i, debug);
	if (i != PAM_SUCCESS) {
		return PAM_SESSION_ERR;
	}

	/* Figure out where the source user's .Xauthority file is. */
	if (getenv(XAUTHENV) != NULL) {
		cookiefile = strdup(getenv(XAUTHENV));
	} else {
		cookiefile = malloc(strlen(rpwd->pw_dir) + 1 +
				    strlen(XAUTHDEF) + 1);
		if (cookiefile == NULL) {
			return PAM_SESSION_ERR;
		}
		strcpy(cookiefile, rpwd->pw_dir);
		strcat(cookiefile, "/");
		strcat(cookiefile, XAUTHDEF);
	}
	if (debug) {
		syslog(LOG_DEBUG, "pam_xauth: reading keys from `%s'",
		       cookiefile);
	}

	/* Read the user's .Xauthority file.  Because the current UID is
	 * the original user's UID, this will only fail if something has
	 * gone wrong, or we have no cookies. */
	if (debug) {
		syslog(LOG_DEBUG, "pam_xauth: running \"%s %s %s %s %s\" as "
		       "%lu/%lu",
		       xauth,
		       "-f",
		       cookiefile,
		       "nlist",
		       display,
		       (unsigned long) getuid(),
		       (unsigned long) getgid());
	}
	if (run_coprocess(NULL, &cookie,
			  getuid(), getgid(),
			  xauth, "-f", cookiefile, "nlist", display,
			  NULL) == 0) {
		/* Check that we got a cookie.  If not, we get creative. */
		if (((cookie == NULL) || (strlen(cookie) == 0)) &&
		    ((strncmp(display, "localhost:", 10) == 0) ||
		     (strncmp(display, "localhost/unix:", 15) == 0))) {
			char *t, *screen;
			size_t tlen, slen;
			/* Free the useless cookie string. */
			if (cookie != NULL) {
				free(cookie);
				cookie = NULL;
			}
			/* Allocate enough space to hold an adjusted name. */
			tlen = strlen(display) + LINE_MAX + 1;
			t = malloc(tlen);
			if (t != NULL) {
				memset(t, 0, tlen);
				if (gethostname(t, tlen - 1) != -1) {
					/* Append the protocol and then the
					 * screen number. */
					if (strlen(t) < tlen - 6) {
						strcat(t, "/unix:");
					}
					screen = strchr(display, ':');
					if (screen != NULL) {
						screen++;
						slen = strlen(screen);
						if (strlen(t) + slen < tlen) {
							strcat(t, screen);
						}
					}
					if (debug) {
						syslog(LOG_DEBUG, "pam_xauth: "
						       "no key for `%s', trying"
						       " `%s'", display, t);
					}
					/* Read the cookie for this display. */
					if (debug) {
						syslog(LOG_DEBUG,
						       "pam_xauth: running "
						       "\"%s %s %s %s %s\" as "
						       "%lu/%lu",
						       xauth,
						       "-f",
						       cookiefile,
						       "nlist",
						       t,
						       (unsigned long) getuid(),
						       (unsigned long) getgid());
					}
					run_coprocess(NULL, &cookie,
						      getuid(), getgid(),
						      xauth, "-f", cookiefile,
						      "nlist", t, NULL);
				}
				free(t);
				t = NULL;
			}
		}

		/* Check that we got a cookie, this time for real. */
		if ((cookie == NULL) || (strlen(cookie) == 0)) {
			if (debug) {
				syslog(LOG_DEBUG, "pam_xauth: no key");
			}
			return PAM_SESSION_ERR;
		}

		/* Generate the environment variable
		 * "XAUTHORITY=<homedir>/filename". */
		xauthority = malloc(strlen(XAUTHENV) + 1 +
				    strlen(tpwd->pw_dir) + 1 +
				    strlen(XAUTHTMP) + 1);
		if (xauthority == NULL) {
			if (debug) {
				syslog(LOG_DEBUG, "pam_xauth: no free memory");
			}
			free(cookiefile);
			free(cookie);
			return PAM_SESSION_ERR;
		}
		strcpy(xauthority, XAUTHENV);
		strcat(xauthority, "=");
		strcat(xauthority, tpwd->pw_dir);
		strcat(xauthority, "/");
		strcat(xauthority, XAUTHTMP);

		/* Generate a new file to hold the data. */
		euid = geteuid();
		setfsuid(tpwd->pw_uid);
		fd = mkstemp(xauthority + strlen(XAUTHENV) + 1);
		setfsuid(euid);
		if (fd == -1) {
			syslog(LOG_ERR, "pam_xauth: error creating "
			       "temporary file `%s': %s",
			       xauthority + strlen(XAUTHENV) + 1,
			       strerror(errno));
			free(cookiefile);
			free(cookie);
			free(xauthority);
			return PAM_SESSION_ERR;
		}
		/* Set permissions on the new file and dispose of the
		 * descriptor. */
		if (fchown(fd, tpwd->pw_uid, tpwd->pw_gid) < 0)
		  syslog (LOG_ERR, "pam_xauth: fchown failed: %m");
		close(fd);

		/* Get a copy of the filename to save as a data item for
		 * removal at session-close time. */
		free(cookiefile);
		cookiefile = strdup(xauthority + strlen(XAUTHENV) + 1);

		/* Save the filename. */
		if (pam_set_data(pamh, DATANAME, cookiefile, cleanup) != PAM_SUCCESS) {
			syslog(LOG_ERR, "pam_xauth: error saving name of "
			       "temporary file `%s'", cookiefile);
			unlink(cookiefile);
			free(xauthority);
			free(cookiefile);
			free(cookie);
			return PAM_SESSION_ERR;
		}

		/* Unset any old XAUTHORITY variable in the environment. */
		if (getenv (XAUTHENV))
		  unsetenv (XAUTHENV);

		/* Set the new variable in the environment. */
		if (pam_putenv (pamh, xauthority) != PAM_SUCCESS)
		  syslog (LOG_DEBUG, "pam_xauth: can't set environment variable '%s'",
			  xauthority);
		putenv (xauthority); /* The environment owns this string now. */

		/* set $DISPLAY in pam handle to make su - work */
		{
		  char *d = (char *) malloc (strlen ("DISPLAY=") +
					     strlen (display) + 1);
		  if (d == NULL)
		    {
		      syslog (LOG_DEBUG, "pam_xauth: memory exhausted\n");
		      return PAM_SESSION_ERR;
		    }
		  strcpy (d, "DISPLAY=");
		  strcat (d, display);

		  if (pam_putenv (pamh, d) != PAM_SUCCESS)
		    syslog (LOG_DEBUG,
			    "pam_xauth: can't set environment variable '%s'",
			    d);
		  free (d);
		}

		/* Merge the cookie we read before into the new file. */
		if (debug) {
			syslog(LOG_DEBUG, "pam_xauth: writing key `%s' to "
			       "temporary file `%s'", cookie, cookiefile);
		}
		if (debug) {
			syslog(LOG_DEBUG,
			       "pam_xauth: running \"%s %s %s %s %s\" as "
			       "%lu/%lu",
			       xauth,
			       "-f",
			       cookiefile,
			       "nmerge",
			       "-",
			       (unsigned long) tpwd->pw_uid,
			       (unsigned long) tpwd->pw_gid);
		}
		run_coprocess(cookie, &tmp,
			      tpwd->pw_uid, tpwd->pw_gid,
			      xauth, "-f", cookiefile, "nmerge", "-", NULL);

		/* We don't need to keep a copy of these around any more. */
		free(cookie);
		cookie = NULL;
	}
	return PAM_SUCCESS;
}

int
pam_sm_close_session(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
	void *cookiefile;
	int i, debug = 0;

	/* Parse arguments.  We don't understand many, so no sense in breaking
	 * this into a separate function. */
	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "debug") == 0) {
			debug = 1;
			continue;
		}
		if (strncmp(argv[i], "xauthpath=", 10) == 0) {
			continue;
		}
		if (strncmp(argv[i], "systemuser=", 11) == 0) {
			continue;
		}
		if (strncmp(argv[i], "targetuser=", 11) == 0) {
			continue;
		}
		syslog(LOG_WARNING, "pam_xauth: unrecognized option `%s'",
		       argv[i]);
	}

	/* Try to retrieve the name of a file we created when the session was
	 * opened. */
	if (pam_get_data(pamh, DATANAME, (const void**) &cookiefile) == PAM_SUCCESS) {
		/* We'll only try to remove the file once. */
		if (strlen((char*)cookiefile) > 0) {
			if (debug) {
				syslog(LOG_DEBUG, "pam_xauth: removing `%s'",
				       (char*)cookiefile);
			}
			unlink((char*)cookiefile);
			*((char*)cookiefile) = '\0';
		}
	}
	return PAM_SUCCESS;
}
