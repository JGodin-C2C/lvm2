/*
 * Copyright (C) 2003-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2015 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <time.h>

#include "tools.h"
#include "polldaemon.h"
#include "lvm2cmdline.h"
#include "lvmpolld-client.h"

#define WAIT_AT_LEAST_NANOSECS 100000

progress_t poll_mirror_progress(struct cmd_context *cmd,
				struct logical_volume *lv, const char *name,
				struct daemon_parms *parms)
{
	dm_percent_t segment_percent = DM_PERCENT_0, overall_percent = DM_PERCENT_0;
	uint32_t event_nr = 0;

	if (!lv_is_mirrored(lv) ||
	    !lv_mirror_percent(cmd, lv, !parms->interval, &segment_percent,
			       &event_nr) ||
	    (segment_percent == DM_PERCENT_INVALID)) {
		log_error("ABORTING: Mirror percentage check failed.");
		return PROGRESS_CHECK_FAILED;
	}

	overall_percent = copy_percent(lv);
	if (parms->progress_display)
		log_print_unless_silent("%s: %s: %.1f%%", name, parms->progress_title,
					dm_percent_to_float(overall_percent));
	else
		log_verbose("%s: %s: %.1f%%", name, parms->progress_title,
			    dm_percent_to_float(overall_percent));

	if (segment_percent != DM_PERCENT_100)
		return PROGRESS_UNFINISHED;

	if (overall_percent == DM_PERCENT_100)
		return PROGRESS_FINISHED_ALL;

	return PROGRESS_FINISHED_SEGMENT;
}

struct volume_group *poll_get_copy_vg(struct cmd_context *cmd,
				      const char *name,
				      const char *uuid __attribute__((unused)),
				      uint32_t flags)
{
	dev_close_all();

	if (name && !strchr(name, '/'))
		return vg_read(cmd, name, NULL, flags);

	/* 'name' is the full LV name; must extract_vgname() */
	return vg_read(cmd, extract_vgname(cmd, name), NULL, flags);
}

struct logical_volume *poll_get_copy_lv(struct cmd_context *cmd __attribute__((unused)),
					struct volume_group *vg,
					const char *name,
					const char *uuid,
					uint64_t lv_type)
{
	struct logical_volume *lv = find_lv(vg, name);

	if (!lv || (uuid && strcmp(uuid, (char *)&lv->lvid)) || (lv_type && !(lv->status & lv_type)))
		return NULL;

	return lv;
}

static int _check_lv_status(struct cmd_context *cmd,
			    struct volume_group *vg,
			    struct logical_volume *lv,
			    const char *name, struct daemon_parms *parms,
			    int *finished)
{
	struct dm_list *lvs_changed;
	progress_t progress;

	/* By default, caller should not retry */
	*finished = 1;

	if (parms->aborting) {
		if (!(lvs_changed = lvs_using_lv(cmd, vg, lv))) {
			log_error("Failed to generate list of copied LVs: "
				  "can't abort.");
			return 0;
		}
		if (!parms->poll_fns->finish_copy(cmd, vg, lv, lvs_changed))
			return_0;

		return 1;
	}

	progress = parms->poll_fns->poll_progress(cmd, lv, name, parms);
	if (progress == PROGRESS_CHECK_FAILED)
		return_0;

	if (progress == PROGRESS_UNFINISHED) {
		/* The only case the caller *should* try again later */
		*finished = 0;
		return 1;
	}

	if (!(lvs_changed = lvs_using_lv(cmd, vg, lv))) {
		log_error("ABORTING: Failed to generate list of copied LVs");
		return 0;
	}

	/* Finished? Or progress to next segment? */
	if (progress == PROGRESS_FINISHED_ALL) {
		if (!parms->poll_fns->finish_copy(cmd, vg, lv, lvs_changed))
			return_0;
	} else {
		if (parms->poll_fns->update_metadata &&
		    !parms->poll_fns->update_metadata(cmd, vg, lv, lvs_changed, 0)) {
			log_error("ABORTING: Segment progression failed.");
			parms->poll_fns->finish_copy(cmd, vg, lv, lvs_changed);
			return 0;
		}
		*finished = 0;	/* Another segment */
	}

	return 1;
}

static void _nanosleep(unsigned secs, unsigned allow_zero_time)
{
	struct timespec wtime = {
		.tv_sec = secs,
	};

	if (!secs && !allow_zero_time)
		wtime.tv_nsec = WAIT_AT_LEAST_NANOSECS;

	while (!nanosleep(&wtime, &wtime) && errno == EINTR) {}
}

static void _sleep_and_rescan_devices(struct daemon_parms *parms)
{
	if (parms->interval && !parms->aborting) {
		_nanosleep(parms->interval, 1);
		/* Devices might have changed while we slept */
		init_full_scan_done(0);
	}
}

int wait_for_single_lv(struct cmd_context *cmd, struct poll_operation_id *id,
		       struct daemon_parms *parms)
{
	struct volume_group *vg;
	struct logical_volume *lv;
	int finished = 0;

	/* Poll for completion */
	while (!finished) {
		if (parms->wait_before_testing)
			_sleep_and_rescan_devices(parms);

		/* Locks the (possibly renamed) VG again */
		vg = parms->poll_fns->get_copy_vg(cmd, id->vg_name, NULL, READ_FOR_UPDATE);
		if (vg_read_error(vg)) {
			release_vg(vg);
			log_error("ABORTING: Can't reread VG for %s.", id->display_name);
			/* What more could we do here? */
			return 0;
		}

		lv = parms->poll_fns->get_copy_lv(cmd, vg, id->lv_name, id->uuid, parms->lv_type);

		if (!lv && parms->lv_type == PVMOVE) {
			log_print_unless_silent("%s: no pvmove in progress - already finished or aborted.",
						id->display_name);
			unlock_and_release_vg(cmd, vg, vg->name);
			return 1;
		}

		if (!lv) {
			log_error("ABORTING: Can't find LV in %s for %s.",
				  vg->name, id->display_name);
			unlock_and_release_vg(cmd, vg, vg->name);
			return 0;
		}

		/*
		 * If the LV is not active locally, the kernel cannot be
		 * queried for its status.  We must exit in this case.
		 */
		if (!lv_is_active_locally(lv)) {
			log_print_unless_silent("%s: Interrupted: No longer active.", id->display_name);
			unlock_and_release_vg(cmd, vg, vg->name);
			return 1;
		}

		if (!_check_lv_status(cmd, vg, lv, id->display_name, parms, &finished)) {
			unlock_and_release_vg(cmd, vg, vg->name);
			return_0;
		}

		unlock_and_release_vg(cmd, vg, vg->name);

		/*
		 * FIXME Sleeping after testing, while preferred, also works around
		 * unreliable "finished" state checking in _percent_run.  If the
		 * above _check_lv_status is deferred until after the first sleep it
		 * may be that a polldaemon will run without ever completing.
		 *
		 * This happens when one snapshot-merge polldaemon is racing with
		 * another (polling the same LV).  The first to see the LV status
		 * reach the "finished" state will alter the LV that the other
		 * polldaemon(s) are polling.  These other polldaemon(s) can then
		 * continue polling an LV that doesn't have a "status".
		 */
		if (!parms->wait_before_testing && !finished)
			_sleep_and_rescan_devices(parms);
	}

	return 1;
}

struct poll_id_list {
	struct dm_list list;
	struct poll_operation_id *id;
};

static struct poll_operation_id *copy_poll_operation_id(struct dm_pool *mem,
							const struct poll_operation_id *id)
{
	struct poll_operation_id *copy;

	if (!id)
		return_NULL;

	copy = (struct poll_operation_id *) dm_pool_alloc(mem, sizeof(struct poll_operation_id));
	if (!copy) {
		log_error("Poll operation ID allocation failed.");
		return NULL;
	}

	copy->display_name = id->display_name ? dm_pool_strdup(mem, id->display_name) : NULL;
	copy->lv_name = id->lv_name ? dm_pool_strdup(mem, id->lv_name) : NULL;
	copy->vg_name = id->vg_name ? dm_pool_strdup(mem, id->vg_name) : NULL;
	copy->uuid = id->uuid ? dm_pool_strdup(mem, id->uuid) : NULL;

	if (!copy->display_name || !copy->lv_name || !copy->vg_name || !copy->uuid) {
		log_error("Failed to copy one or more poll_operation_id members.");
		return NULL;
	}

	return copy;
}

static struct poll_id_list* poll_id_list_create(struct dm_pool *mem,
						const struct poll_operation_id *id)
{
	struct poll_id_list *idl = (struct poll_id_list *) dm_pool_alloc(mem, sizeof(struct poll_id_list));

	if (!idl) {
		log_error("Poll ID list allocation failed.");
		return NULL;
	}

	if (!(idl->id = copy_poll_operation_id(mem, id))) {
		dm_pool_free(mem, idl);
		return NULL;
	}

	return idl;
}

static int _poll_vg(struct cmd_context *cmd, const char *vgname,
		    struct volume_group *vg, struct processing_handle *handle)
{
	struct daemon_parms *parms;
	struct lv_list *lvl;
	struct dm_list idls;
	struct poll_id_list *idl;
	struct poll_operation_id id;
	struct logical_volume *lv;
	int finished;

	if (!handle || !(parms = (struct daemon_parms *) handle->custom_handle)) {
		log_error(INTERNAL_ERROR "Handle is undefined.");
		return ECMD_FAILED;
	}

	dm_list_init(&idls);

	/*
	 * first iterate all LVs in a VG and collect LVs suitable
	 * for polling (or an abort) which takes place below
	 */
	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;
		if (!(lv->status & parms->lv_type))
			continue;
		id.display_name = parms->poll_fns->get_copy_name_from_lv(lv);
		if (!id.display_name && !parms->aborting)
			continue;

		if (!id.display_name) {
			log_error("Device name for LV %s not found in metadata. "
				  "(unfinished pvmove mirror removal?)", display_lvname(lv));
			goto err;
		}

		/* FIXME Need to do the activation from _set_up_pvmove here
		 *       if it's not running and we're not aborting. */
		if (!lv_is_active(lv)) {
			log_print_unless_silent("%s: Skipping inactive LV. Try lvchange or vgchange.", id.display_name);
			continue;
		}

		id.lv_name = lv->name;
		id.vg_name = vg->name;
		id.uuid = lv->lvid.s;

		idl = poll_id_list_create(cmd->mem, &id);
		if (!idl) {
			log_error("Failed to create poll_id_list.");
			goto err;
		}

		dm_list_add(&idls, &idl->list);
	}

	/* perform the poll operation on LVs collected in previous cycle */
	dm_list_iterate_items(idl, &idls) {
		lv = parms->poll_fns->get_copy_lv(cmd, vg, idl->id->lv_name, idl->id->uuid, parms->lv_type);
		if (lv && _check_lv_status(cmd, vg, lv, idl->id->display_name, parms, &finished) && !finished)
			parms->outstanding_count++;
	}

err:
	if (!dm_list_empty(&idls))
		dm_pool_free(cmd->mem, dm_list_item(dm_list_first(&idls), struct poll_id_list));

	return ECMD_PROCESSED;
}

static void _poll_for_all_vgs(struct cmd_context *cmd,
			      struct processing_handle *handle)
{
	struct daemon_parms *parms = (struct daemon_parms *) handle->custom_handle;

	while (1) {
		parms->outstanding_count = 0;
		process_each_vg(cmd, 0, NULL, READ_FOR_UPDATE, handle, _poll_vg);
		if (!parms->outstanding_count)
			break;
		_nanosleep(parms->interval, 1);
	}
}

#ifdef LVMPOLLD_SUPPORT
struct poll_lv_list {
	struct dm_list list;
	char *uuid;
	char *name;
};

typedef struct {
	struct daemon_parms *parms;
	struct dm_list plvs;
} lvmpolld_parms_t;

static int report_progress(struct cmd_context *cmd, struct poll_operation_id *id,
			   struct daemon_parms *parms)
{
	struct volume_group *vg;
	struct logical_volume *lv;

	vg = parms->poll_fns->get_copy_vg(cmd, id->vg_name, NULL, 1);
	if (vg_read_error(vg)) {
		release_vg(vg);
		log_error("Can't reread VG for %s", id->display_name);
		return 0;
	}

	lv = parms->poll_fns->get_copy_lv(cmd, vg, id->lv_name, id->uuid, parms->lv_type);
	if (!lv && parms->lv_type == PVMOVE) {
		log_print_unless_silent("%s: no pvmove in progress - already finished or aborted.",
					id->display_name);
		unlock_and_release_vg(cmd, vg, vg->name);
		return 1;
	}

	if (!lv) {
		log_warn("Can't find LV in %s for %s. Already finished or removed.",
			  vg->name, id->display_name);
		unlock_and_release_vg(cmd, vg, vg->name);
		return 1;
	}

	if (!lv_is_active_locally(lv)) {
		log_print_unless_silent("%s: Interrupted: No longer active.", id->display_name);
		unlock_and_release_vg(cmd, vg, vg->name);
		return 1;
	}

	if (parms->poll_fns->poll_progress(cmd, lv, id->display_name, parms) == PROGRESS_CHECK_FAILED) {
		unlock_and_release_vg(cmd, vg, vg->name);
		return_0;
	}

	unlock_and_release_vg(cmd, vg, vg->name);

	return 1;
}

/* FIXME: this requires audit after code modifications due to bug in pvmove handling */
/* FIXME: this requires audit after code modifications due to bug in pvmove handling */
/* FIXME: this requires audit after code modifications due to bug in pvmove handling */
/* FIXME: this requires audit after code modifications due to bug in pvmove handling */
/* FIXME: this requires audit after code modifications due to bug in pvmove handling */
static int _lvmpolld_init_poll_vg(struct cmd_context *cmd, const char *vgname,
			          struct volume_group *vg, struct processing_handle *handle)
{
	int r;
	struct lv_list *lvl;
	struct logical_volume *lv;
	struct poll_lv_list *plv;
	union lvid lvid;
	lvmpolld_parms_t *lpdp = (lvmpolld_parms_t *) handle->custom_handle;
	struct poll_operation_id id = { 0 };

	dm_list_iterate_items(lvl, &vg->lvs) {
		lv = lvl->lv;
		if (!(lv->status & lpdp->parms->lv_type))
			continue;
		id.display_name = lpdp->parms->poll_fns->get_copy_name_from_lv(lv);
		if (!id.display_name && !lpdp->parms->aborting)
			continue;

		memset(&lvid, 0, sizeof(union lvid));
		memcpy(&lvid, &lv->lvid, sizeof(*(lvid.id)));

		if (!lvid.s) {
			/* TODO: rephrase this log message */
			log_print_unless_silent("Failed to extract vgid from VG including PV: %s", id.display_name);
			continue;
		}

		id.vg_name = lv->vg->name;
		id.lv_name = lv->name;

		r = lvmpolld_poll_init(cmd,lv->vg->name, lv->name, lvid.s, lpdp->parms->lv_type,
				       lpdp->parms->interval, lpdp->parms->aborting);

		if (r && !lpdp->parms->background) {
			plv = (struct poll_lv_list *) dm_malloc(sizeof(struct poll_lv_list));
			plv->uuid = dm_strdup(lvid.s);
			plv->name = dm_strdup(id.display_name);
			dm_list_add(&lpdp->plvs, &plv->list);
		}
	}

	return ECMD_PROCESSED;
}

static void _lvmpolld_poll_for_all_vgs(struct cmd_context *cmd,
				       struct daemon_parms *parms,
				       struct processing_handle *handle)
{
	int r;
	struct poll_lv_list *plv, *tlv;
	unsigned finished;
	lvmpolld_parms_t lpdp = {
		.parms = parms
	};
	struct poll_operation_id id = { 0 };

	dm_list_init(&lpdp.plvs);

	handle->custom_handle = &lpdp;

	process_each_vg(cmd, 0, NULL, 0, handle, _lvmpolld_init_poll_vg);

	while (!dm_list_empty(&lpdp.plvs)) {
		dm_list_iterate_items_safe(plv, tlv, &lpdp.plvs) {
			id.display_name = plv->name;
			id.uuid = plv->uuid;
			r = lvmpolld_request_info(id.uuid, lpdp.parms->aborting,
						  &finished);
			if (!r || finished) {
				dm_list_del(&plv->list);
				dm_free(plv->uuid);
				dm_free(plv->name);
				dm_free(plv);
			}
			else
				report_progress(cmd, &id, lpdp.parms);
		}

		_nanosleep(lpdp.parms->interval, 0);
	}
}

static int _lvmpoll_daemon(struct cmd_context *cmd, struct poll_operation_id *id,
			   struct daemon_parms *parms)
{
	int r;
	struct processing_handle *handle = NULL;
	unsigned finished = 0;

	if (id) {
		r = lvmpolld_poll_init(cmd, id->vg_name, id->lv_name, id->uuid,
				       parms->lv_type, parms->interval,
				       parms->aborting);

		while (r && !parms->background && !finished) {
			if (!(r = lvmpolld_request_info(id->uuid, parms->aborting, &finished)))
				break;
			if (!finished && !(r = report_progress(cmd, id, parms)))
				break;

			_nanosleep(parms->interval, 0);
		}

		return r ? ECMD_PROCESSED : ECMD_FAILED;
	} else {
		/* process all in-flight operations */
		if (!(handle = init_processing_handle(cmd))) {
			log_error("Failed to initialize processing handle.");
			return ECMD_FAILED;
		} else {
			_lvmpolld_poll_for_all_vgs(cmd, parms, handle);
			destroy_processing_handle(cmd, handle);
			return ECMD_PROCESSED;
		}
	}
}
#else
#	define _lvmpoll_daemon(cmd, id, parms) (ECMD_FAILED)
#endif /* LVMPOLLD_SUPPORT */

/*
 * Only allow *one* return from poll_daemon() (the parent).
 * If there is a child it must exit (ignoring the memory leak messages).
 * - 'background' is advisory so a child polldaemon may not be used even
 *   if it was requested.
 */
static int _poll_daemon(struct cmd_context *cmd, struct poll_operation_id *id,
			struct daemon_parms *parms)
{
	struct processing_handle *handle = NULL;
	int daemon_mode = 0;
	int ret = ECMD_PROCESSED;

	if (parms->background) {
		daemon_mode = become_daemon(cmd, 0);
		if (daemon_mode == 0)
			return ECMD_PROCESSED;	    /* Parent */
		else if (daemon_mode == 1)
			parms->progress_display = 0; /* Child */
		/* FIXME Use wait_event (i.e. interval = 0) and */
		/*       fork one daemon per copy? */
	}

	/*
	 * Process one specific task or all incomplete tasks?
	 */
	if (id) {
		if (!wait_for_single_lv(cmd, id, parms)) {
			stack;
			ret = ECMD_FAILED;
		}
	} else {
		if (!parms->interval)
			parms->interval = find_config_tree_int(cmd, activation_polling_interval_CFG, NULL);
		if (!(handle = init_processing_handle(cmd))) {
			log_error("Failed to initialize processing handle.");
			ret = ECMD_FAILED;
		} else {
			handle->custom_handle = parms;
			_poll_for_all_vgs(cmd, handle);
		}
	}

	if (parms->background && daemon_mode == 1) {
		destroy_processing_handle(cmd, handle);
		/*
		 * child was successfully forked:
		 * background polldaemon must not return to the caller
		 * because it will redundantly continue performing the
		 * caller's task (that the parent already performed)
		 */
		/* FIXME Attempt proper cleanup */
		_exit(lvm_return_code(ret));
	}

	destroy_processing_handle(cmd, handle);
	return ret;
}

static int _daemon_parms_init(struct cmd_context *cmd, struct daemon_parms *parms,
			      unsigned background, struct poll_functions *poll_fns,
			      const char *progress_title, uint64_t lv_type)
{
	sign_t interval_sign;

	parms->aborting = arg_is_set(cmd, abort_ARG);
	parms->background = background;
	interval_sign = arg_sign_value(cmd, interval_ARG, SIGN_NONE);
	if (interval_sign == SIGN_MINUS) {
		log_error("Argument to --interval cannot be negative.");
		return 0;
	}
	parms->interval = arg_uint_value(cmd, interval_ARG,
					 find_config_tree_int(cmd, activation_polling_interval_CFG, NULL));
	parms->wait_before_testing = (interval_sign == SIGN_PLUS);
	parms->progress_title = progress_title;
	parms->lv_type = lv_type;
	parms->poll_fns = poll_fns;

	if (parms->interval && !parms->aborting)
		log_verbose("Checking progress %s waiting every %u seconds.",
			    (parms->wait_before_testing ? "after" : "before"),
			    parms->interval);

	parms->progress_display = parms->interval ? 1 : 0;

	return 1;
}

int poll_daemon(struct cmd_context *cmd, unsigned background,
		uint64_t lv_type, struct poll_functions *poll_fns,
		const char *progress_title, struct poll_operation_id *id)
{
	struct daemon_parms parms;

	if (!_daemon_parms_init(cmd, &parms, background, poll_fns, progress_title, lv_type))
		return_EINVALID_CMD_LINE;

	if (lvmpolld_use())
		return _lvmpoll_daemon(cmd, id, &parms);
	else {
		/* classical polling allows only PMVOVE or 0 values */
		parms.lv_type &= PVMOVE;
		return _poll_daemon(cmd, id, &parms);
	}
}
