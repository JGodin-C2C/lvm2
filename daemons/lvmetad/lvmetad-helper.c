/*
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#define _XOPEN_SOURCE 500  /* pthread */
#define _GNU_SOURCE
#define _REENTRANT

#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <poll.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <grp.h>
#include <assert.h>
#include <errno.h>

#include "libudev.h"

#include "tool.h"

#include "daemon-io.h"
#include "daemon-server.h"
#include "daemon-log.h"
#include "lvm-version.h"
#include "lvmetad-internal.h"
#include "lvmetad-client.h"

/*
 * lvmetad_main is the main process that:
 * . forks threads to handle client (command) connections
 * . receives client requests from socket
 * . reads/writes cache state
 * . receives uevent messages
 * . sends lvmetad_helper pvscan requests for uevents
 * . sends lvmetad_helper client requests from lvmetactl
 *
 * lvmetad_helper is the helper process that:
 * . receives pvscan requests from lvmetad_main
 * . forks/execs pvscan commands for each request
 */

#define MAX_AV_COUNT 8

static int _log_debug_stderr;

#define log_debug(fmt, args...) \
do { \
	if (_log_debug_stderr) \
		fprintf(stderr, fmt "\n", ##args); \
} while (0)

static void run_path(struct helper_msg *hm)
{
	char arg[HELPER_ARGS_LEN];
	char *args = hm->args;
	char *av[MAX_AV_COUNT + 1]; /* +1 for NULL */
	int av_count = 0;
	int i, arg_len, args_len;

	for (i = 0; i < MAX_AV_COUNT + 1; i++)
		av[i] = NULL;

	av[av_count++] = strdup(hm->path);

	if (!args[0])
		goto run;

	/* this should already be done, but make sure */
	args[HELPER_ARGS_LEN - 1] = '\0';

	memset(&arg, 0, sizeof(arg));
	arg_len = 0;
	args_len = strlen(args);

	for (i = 0; i < args_len; i++) {
		if (!args[i])
			break;

		if (av_count == MAX_AV_COUNT)
			break;

		if (args[i] == '\\') {
			if (i == (args_len - 1))
				break;
			i++;

			if (args[i] == '\\') {
				arg[arg_len++] = args[i];
				continue;
			}
			if (isspace(args[i])) {
				arg[arg_len++] = args[i];
				continue;
			} else {
				break;
			}
		}

		if (isalnum(args[i]) || ispunct(args[i])) {
			arg[arg_len++] = args[i];
		} else if (isspace(args[i])) {
			if (arg_len)
				av[av_count++] = strdup(arg);

			memset(arg, 0, sizeof(arg));
			arg_len = 0;
		} else {
			break;
		}
	}

	if ((av_count < MAX_AV_COUNT) && arg_len) {
		av[av_count++] = strdup(arg);
	}
run:
	execvp(av[0], av);
}

static int read_from_main(int fd, struct helper_msg *hm)
{
	int rv;
 retry:
	rv = read(fd, hm, sizeof(struct helper_msg));
	if (rv == -1 && errno == EINTR)
		goto retry;

	if (rv != sizeof(struct helper_msg))
		return -1;
	return 0;
}

static int send_to_main(int fd, int type)
{
	struct helper_status hs;
	int rv;

	memset(&hs, 0, sizeof(hs));

	hs.type = type;

	rv = write(fd, &hs, sizeof(hs));

	if (rv == sizeof(hs))
		return 0;
	return -1;
}

#define INACTIVE_TIMEOUT_MS 10000
#define ACTIVE_TIMEOUT_MS 1000

int run_helper(int in_fd, int out_fd, int debug_stderr)
{
	char name[16];
	struct pollfd pollfd;
	struct helper_msg hm;
	unsigned int fork_count = 0;
	unsigned int wait_count = 0;
	int timeout = INACTIVE_TIMEOUT_MS;
	int rv, pid, status;

	_log_debug_stderr = debug_stderr;

	memset(name, 0, sizeof(name));
	sprintf(name, "%s", "lvmetad_helper");
	prctl(PR_SET_NAME, (unsigned long)name, 0, 0, 0);

	memset(&pollfd, 0, sizeof(pollfd));
	pollfd.fd = in_fd;
	pollfd.events = POLLIN;

	/* Tell the main process we've started. */
	send_to_main(out_fd, HELPER_STARTED);

	while (1) {
		rv = poll(&pollfd, 1, timeout);
		if (rv == -1 && errno == EINTR)
			continue;

		if (rv < 0)
			exit(0);

		memset(&hm, 0, sizeof(hm));

		if (pollfd.revents & POLLIN) {
			rv = read_from_main(in_fd, &hm);
			if (rv)
				continue;

			if (hm.type == HELPER_MSG_RUNPATH) {
				pid = fork();
				if (!pid) {
					run_path(&hm);
					exit(-1);
				}

				fork_count++;

				/*
				log_debug("helper fork %d count %d %d %s %s",
					  pid, fork_count, wait_count,
					  hm.path, hm.args);
				*/
			}
		}

		if (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL))
			exit(0);

		/* collect child exits until no more children exist (ECHILD)
		   or none are ready (WNOHANG) */

		while (1) {
			rv = waitpid(-1, &status, WNOHANG);
			if (rv > 0) {
				wait_count++;

				/*
				log_debug("helper wait %d count %d %d",
					  rv, fork_count, wait_count);
				*/
				continue;
			}

			/* no more children to wait for or no children
			   have exited */

			if (rv < 0 && errno == ECHILD)
				timeout = INACTIVE_TIMEOUT_MS;
			else
				timeout = ACTIVE_TIMEOUT_MS;
			break;
		}
	}

	return 0;
}

void close_helper(daemon_state *s)
{
	lvmetad_state *ls = s->private;

	close(ls->helper_pr_fd);
	close(ls->helper_pw_fd);
	ls->helper_pr_fd = -1;
	ls->helper_pw_fd = -1;
	s->helper_fd = -1;
	s->helper_handler = NULL;
}

static void _send_helper_msg(daemon_state *s, struct helper_msg *hm)
{
	lvmetad_state *ls = s->private;
	int rv;

 retry:
	rv = write(ls->helper_pw_fd, hm, sizeof(struct helper_msg));
	if (rv == -1 && errno == EINTR)
		goto retry;

	if (rv == -1 && errno == EAGAIN) {
		return;
	}

	/* helper exited or closed fd, quit using helper */
	if (rv == -1 && errno == EPIPE) {
		ERROR(s, "send_helper EPIPE");
		close_helper(s);
		return;
	}

	if (rv != sizeof(struct helper_msg)) {
		/* this shouldn't happen */
		ERROR(s, "send_helper error %d %d", rv, errno);
		close_helper(s);
		return;
	}
}

/* send a request to helper process */

void send_helper_request(daemon_state *s, request r)
{
	struct helper_msg hm = { 0 };
	char *runpath = daemon_request_str(r, "runpath", NULL);
	char *runargs = daemon_request_str(r, "runargs", NULL);

	hm.type = HELPER_MSG_RUNPATH;
	memcpy(hm.path, runpath, HELPER_PATH_LEN);
	if (runargs)
		memcpy(hm.args, runargs, HELPER_ARGS_LEN);

	_send_helper_msg(s, &hm);
}

static void send_helper_pvscan_cache_dev(daemon_state *s, dev_t devt)
{
	lvmetad_state *ls = s->private;
	struct helper_msg hm = { 0 };

	hm.type = HELPER_MSG_RUNPATH;
	sprintf(hm.path, "pvscan");
	snprintf(hm.args, HELPER_ARGS_LEN-1, "--cache %s --major %d --minor %d",
		 ls->enable_autoactivate ? "-aay" : "",
		 major(devt), minor(devt));

	_send_helper_msg(s, &hm);
}

static void send_helper_pvscan_cache_all(daemon_state *s)
{
	lvmetad_state *ls = s->private;
	struct helper_msg hm = { 0 };

	hm.type = HELPER_MSG_RUNPATH;
	sprintf(hm.path, "pvscan");
	snprintf(hm.args, HELPER_ARGS_LEN-1, "--cache %s",
		 ls->enable_autoactivate ? "-aay" : "");

	_send_helper_msg(s, &hm);
}

/*
 * called in context of main lvmetad process
 * handles a message from helper process
 */

static int helper_handler(daemon_state *s)
{
	lvmetad_state *ls = s->private;
	struct helper_status hs;
	int rv;

	memset(&hs, 0, sizeof(hs));

	rv = read(ls->helper_pr_fd, &hs, sizeof(hs));
	if (!rv || rv == -EAGAIN)
		return -1;
	if (rv < 0) {
		ERROR(s, "handle_helper rv %d errno %d", rv, errno);
		goto fail;
	}
	if (rv != sizeof(hs)) {
		ERROR(s, "handle_helper recv size %d", rv);
		goto fail;
	}

	DEBUGLOG(s, "helper message type %d status %d", hs.type, hs.status);

	/*
	 * Run initial pvscan --cache to populate lvmetad cache.
	 *
	 * (Upon receiving HELPER_STARTED we know that the helper
	 * is ready to handle to running commands.)
	 *
	 * The udev monitor is enabled before this, so there should
	 * be no gap where new devs could be missed.  It's possible
	 * that new devs added during lvmetad startup could be scanned
	 * by this initial pvscan --cache, and then scanned again
	 * individually because of the monitor.  This possible repetition
	 * is harmless.
	 */
	if (hs.type == HELPER_STARTED)
		send_helper_pvscan_cache_all(s);

	return 0;

 fail:
	ERROR(s, "close helper connection");
	close_helper(s);
	return -1;
}

/* create helper process */

int setup_helper(daemon_state *s)
{
	lvmetad_state *ls = s->private;
	int pid;
	int pw_fd = -1; /* parent write */
	int cr_fd = -1; /* child read */
	int pr_fd = -1; /* parent read */
	int cw_fd = -1; /* child write */
	int pfd[2];

	/* don't allow the main daemon thread to block */
	if (pipe2(pfd, O_NONBLOCK | O_CLOEXEC))
		return -errno;

	/* only available on rhel7 */
	/* fcntl(pfd[1], F_SETPIPE_SZ, 1024*1024); */

	cr_fd = pfd[0];
	pw_fd = pfd[1];

	if (pipe2(pfd, O_NONBLOCK | O_CLOEXEC)) {
		close(cr_fd);
		close(pw_fd);
		return -errno;
	}

	pr_fd = pfd[0];
	cw_fd = pfd[1];

	pid = fork();
	if (pid < 0) {
		close(cr_fd);
		close(pw_fd);
		close(pr_fd);
		close(cw_fd);
		return -errno;
	}

	if (pid) {
		close(cr_fd);
		close(cw_fd);
		ls->helper_pw_fd = pw_fd;
		ls->helper_pr_fd = pr_fd;
		ls->helper_pid = pid;
		s->helper_fd = pr_fd; /* libdaemon uses helper_fd in select */
		s->helper_handler = helper_handler;
		return 0;
	} else {
		close(pr_fd);
		close(pw_fd);
		run_helper(cr_fd, cw_fd, (s->foreground && strstr(ls->log_config, "debug")));
		exit(0);
	}
}

/*
 * called in context of main lvmetad process
 * handles a message from udev monitor
 * sends a message to lvmetad helper to scan a device
 */

static int monitor_handler(daemon_state *s)
{
	lvmetad_state *ls = s->private;
	struct udev_device *dev;
	const char *name;
	dev_t devt;

	dev = udev_monitor_receive_device(ls->udev_mon);
	if (!dev)
		return 0;

	name = udev_device_get_devnode(dev);
	devt = udev_device_get_devnum(dev);

	DEBUGLOG(s, "monitor scan %d:%d %s", major(devt), minor(devt), name ?: "");
	send_helper_pvscan_cache_dev(s, devt);

	udev_device_unref(dev);
	return 0;
}

/* create udev monitor */

void setup_udev_monitor(daemon_state *s)
{
	lvmetad_state *ls = s->private;
	int fd;
	int ret;

	/* FIXME: add error handling/cleanup */

	ls->udevh = udev_new();
	if (!ls->udevh) {
		ERROR(s, "Failed to monitor udev: new.");
		return;
	}

	ls->udev_mon = udev_monitor_new_from_netlink(ls->udevh, "udev");
	if (!ls->udev_mon) {
		ERROR(s, "Failed to monitor udev: netlink.");
		return;
	}

	ret = udev_monitor_filter_add_match_subsystem_devtype(ls->udev_mon, "block", "disk");
	if (ret < 0) {
		ERROR(s, "Failed to monitor udev: devtype.");
		return;
	}

	ret = udev_monitor_filter_add_match_tag(ls->udev_mon, "LVM_DO_PVSCAN");
	if (ret < 0) {
		ERROR(s, "Failed to monitor udev: tag.");
		return;
	}

	ret = udev_monitor_enable_receiving(ls->udev_mon);
	if (ret < 0) {
		ERROR(s, "Failed to monitor udev: receive.");
		return;
	}

	/* udev_monitor_set_receive_buffer_size? */

	fd = udev_monitor_get_fd(ls->udev_mon);
	if (fd < 0) {
		ERROR(s, "Failed to monitor udev: fd.");
		return;
	}

	ls->udev_fd = fd;
	s->monitor_fd = fd;
	s->monitor_handler = monitor_handler;
}

