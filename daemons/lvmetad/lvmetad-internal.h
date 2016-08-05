/*
 * Copyright (C) 2016 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 */

#ifndef _LVM_LVMETAD_INTERNAL_H
#define _LVM_LVMETAD_INTERNAL_H

#define CMD_NAME_SIZE 32

typedef struct {
	daemon_idle *idle;
	log_state *log; /* convenience */
	const char *log_config;
	int enable_udev_monitor;
	int enable_autoactivate;

	struct dm_hash_table *pvid_to_pvmeta;
	struct dm_hash_table *device_to_pvid; /* shares locks with above */

	struct dm_hash_table *vgid_to_metadata;
	struct dm_hash_table *vgid_to_vgname;
	struct dm_hash_table *vgid_to_outdated_pvs;
	struct dm_hash_table *vgid_to_info;
	struct dm_hash_table *vgname_to_vgid;
	struct dm_hash_table *pvid_to_vgid;
	char token[128];
	char update_cmd[CMD_NAME_SIZE];
	int update_pid;
	int update_timeout;
	uint64_t update_begin;
	uint32_t flags; /* GLFL_ */
	pthread_mutex_t token_lock;
	pthread_mutex_t info_lock;
	pthread_rwlock_t cache_lock;

	int helper_pid;
	int helper_pw_fd; /* parent write to send message to helper */
	int helper_pr_fd; /* parent read to recv message from helper */

	struct udev *udevh;
	struct udev_monitor *udev_mon;
	int udev_fd;
} lvmetad_state;

/*
 * helper process
 * recvs 512 byte helper_msg on in_fd
 * sends 4 byte helper_status on out_fd
 */

/* max length of path and args, includes terminate \0 byte */

#define HELPER_PATH_LEN    128
#define HELPER_ARGS_LEN    128
#define HELPER_MSG_LEN     512
#define HELPER_MSG_RUNPATH 1

struct helper_msg {
	uint8_t type;
	uint8_t pad1;
	uint16_t pad2;
	uint32_t flags;
	int pid;
	int unused;
	char path[HELPER_PATH_LEN]; /* 128 */
	char args[HELPER_ARGS_LEN]; /* 128 */
	char pad[240];
};

#define HELPER_STARTED 1

struct helper_status {
	uint8_t type;
	uint8_t status;
	uint16_t len;
};

void close_helper(daemon_state *s);
void send_helper_request(daemon_state *s, request r);
int setup_helper(daemon_state *s);
void setup_udev_monitor(daemon_state *s);

#endif
