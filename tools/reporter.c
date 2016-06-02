/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2009 Red Hat, Inc. All rights reserved.
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

#include "tools.h"

#include "report.h"

typedef enum {
	REPORT_IDX_NULL = -1,
	REPORT_IDX_SINGLE,
	REPORT_IDX_STATUS,
	REPORT_IDX_FULL_VGS,
	REPORT_IDX_FULL_LVS,
	REPORT_IDX_FULL_PVS,
	REPORT_IDX_FULL_PVSEGS,
	REPORT_IDX_FULL_SEGS,
	REPORT_IDX_COUNT
} report_idx_t;

struct single_report_args {
	report_type_t report_type;
	char report_prefix[32];
	int args_are_pvs;
	const char *keys;
	const char *options;
	const char *fields_to_compact;
	const char *selection;
};

/* TODO: configure these common report args only once per cmd */
struct report_args {
	int argc;
	char **argv;
	int aligned;
	int buffered;
	int headings;
	int field_prefixes;
	int quoted;
	int columns_as_rows;
	const char *separator;
	struct volume_group *full_report_vg;
	struct single_report_args single_args[REPORT_IDX_COUNT];
};

static int _process_each_devtype(struct cmd_context *cmd, int argc,
				 struct processing_handle *handle)
{
	if (argc)
		log_warn("WARNING: devtypes currently ignores command line arguments.");

	if (!report_devtypes(handle->custom_handle))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

static int _vgs_single(struct cmd_context *cmd __attribute__((unused)),
		       const char *vg_name, struct volume_group *vg,
		       struct processing_handle *handle)
{
	struct selection_handle *sh = handle->selection_handle;

	if (!report_object(sh ? : handle->custom_handle, sh != NULL,
			   vg, NULL, NULL, NULL, NULL, NULL, NULL))
		return_ECMD_FAILED;

	check_current_backup(vg);

	return ECMD_PROCESSED;
}

static void _choose_lv_segment_for_status_report(const struct logical_volume *lv, const struct lv_segment **lv_seg)
{
	/*
	 * By default, take the first LV segment to report status for.
	 * If there's any other specific segment that needs to be
	 * reported instead for the LV, choose it here and assign it
	 * to lvdm->seg_status->seg. This is the segment whose
	 * status line will be used for report exactly.
	 */
	*lv_seg = first_seg(lv);
}

static int _do_info_and_status(struct cmd_context *cmd,
				const struct logical_volume *lv,
				const struct lv_segment *lv_seg,
				struct lv_with_info_and_seg_status *status,
				int do_info, int do_status)
{
	unsigned use_layer = lv_is_thin_pool(lv) ? 1 : 0;

	status->lv = lv;

	if (lv_is_historical(lv))
		return 1;

	if (do_status) {
		if (!(status->seg_status.mem = dm_pool_create("reporter_pool", 1024)))
			return_0;
		if (!lv_seg)
			_choose_lv_segment_for_status_report(lv, &lv_seg);
		if (do_info) {
			/* both info and status */
			status->info_ok = lv_info_with_seg_status(cmd, lv, lv_seg, use_layer, status, 1, 1);
			/* for inactive thin-pools reset lv info struct */
			if (use_layer && status->info_ok &&
			    !lv_info(cmd, lv, 0, NULL, 0, 0))
				memset(&status->info,  0, sizeof(status->info));
		} else
			/* status only */
			status->info_ok = lv_status(cmd, lv_seg, use_layer, &status->seg_status);
	} else if (do_info)
		/* info only */
		status->info_ok = lv_info(cmd, lv, use_layer, &status->info, 1, 1);

	return 1;
}

static int _do_lvs_with_info_and_status_single(struct cmd_context *cmd,
					       const struct logical_volume *lv,
					       int do_info, int do_status,
					       struct processing_handle *handle)
{
	struct selection_handle *sh = handle->selection_handle;
	struct lv_with_info_and_seg_status status = {
		.seg_status.type = SEG_STATUS_NONE
	};
	int r = ECMD_FAILED;

	if (!_do_info_and_status(cmd, lv, NULL, &status, do_info, do_status))
		goto_out;

	if (!report_object(sh ? : handle->custom_handle, sh != NULL,
			   lv->vg, lv, NULL, NULL, NULL, &status, NULL))
		goto out;

	r = ECMD_PROCESSED;
out:
	if (status.seg_status.mem)
		dm_pool_destroy(status.seg_status.mem);

	return r;
}

static int _lvs_single(struct cmd_context *cmd, struct logical_volume *lv,
		       struct processing_handle *handle)
{
	return _do_lvs_with_info_and_status_single(cmd, lv, 0, 0, handle);
}

static int _lvs_with_info_single(struct cmd_context *cmd, struct logical_volume *lv,
				 struct processing_handle *handle)
{
	return _do_lvs_with_info_and_status_single(cmd, lv, 1, 0, handle);
}

static int _lvs_with_status_single(struct cmd_context *cmd, struct logical_volume *lv,
				   struct processing_handle *handle)
{
	return _do_lvs_with_info_and_status_single(cmd, lv, 0, 1, handle);
}

static int _lvs_with_info_and_status_single(struct cmd_context *cmd, struct logical_volume *lv,
					    struct processing_handle *handle)
{
	return _do_lvs_with_info_and_status_single(cmd, lv, 1, 1, handle);
}

static int _do_segs_with_info_and_status_single(struct cmd_context *cmd,
						const struct lv_segment *seg,
						int do_info, int do_status,
						struct processing_handle *handle)
{
	struct selection_handle *sh = handle->selection_handle;
	struct lv_with_info_and_seg_status status = {
		.seg_status.type = SEG_STATUS_NONE
	};
	int r = ECMD_FAILED;

	if (!_do_info_and_status(cmd, seg->lv, seg, &status, do_info, do_status))
		goto_out;

	if (!report_object(sh ? : handle->custom_handle, sh != NULL,
			   seg->lv->vg, seg->lv, NULL, seg, NULL, &status, NULL))
	goto_out;

	r = ECMD_PROCESSED;
out:
	if (status.seg_status.mem)
		dm_pool_destroy(status.seg_status.mem);

	return r;
}

static int _segs_single(struct cmd_context *cmd, struct lv_segment *seg,
			struct processing_handle *handle)
{
	return _do_segs_with_info_and_status_single(cmd, seg, 0, 0, handle);
}

static int _segs_with_info_single(struct cmd_context *cmd, struct lv_segment *seg,
				  struct processing_handle *handle)
{
	return _do_segs_with_info_and_status_single(cmd, seg, 1, 0, handle);
}

static int _segs_with_status_single(struct cmd_context *cmd, struct lv_segment *seg,
				    struct processing_handle *handle)
{
	return _do_segs_with_info_and_status_single(cmd, seg, 0, 1, handle);
}

static int _segs_with_info_and_status_single(struct cmd_context *cmd, struct lv_segment *seg,
					     struct processing_handle *handle)
{
	return _do_segs_with_info_and_status_single(cmd, seg, 1, 1, handle);
}

static int _lvsegs_single(struct cmd_context *cmd, struct logical_volume *lv,
			  struct processing_handle *handle)
{
	if (!arg_count(cmd, all_ARG) && !lv_is_visible(lv))
		return ECMD_PROCESSED;

	return process_each_segment_in_lv(cmd, lv, handle, _segs_single);
}

static int _lvsegs_with_info_single(struct cmd_context *cmd, struct logical_volume *lv,
				    struct processing_handle *handle)
{
	if (!arg_count(cmd, all_ARG) && !lv_is_visible(lv))
		return ECMD_PROCESSED;

	return process_each_segment_in_lv(cmd, lv, handle, _segs_with_info_single);
}

static int _lvsegs_with_status_single(struct cmd_context *cmd, struct logical_volume *lv,
				      struct processing_handle *handle)
{
	if (!arg_count(cmd, all_ARG) && !lv_is_visible(lv))
		return ECMD_PROCESSED;

	return process_each_segment_in_lv(cmd, lv, handle, _segs_with_status_single);
}

static int _lvsegs_with_info_and_status_single(struct cmd_context *cmd, struct logical_volume *lv,
					       struct processing_handle *handle)
{
	if (!arg_count(cmd, all_ARG) && !lv_is_visible(lv))
		return ECMD_PROCESSED;

	return process_each_segment_in_lv(cmd, lv, handle, _segs_with_info_and_status_single);
}

static int _do_pvsegs_sub_single(struct cmd_context *cmd,
				 struct volume_group *vg,
				 struct pv_segment *pvseg,
				 int do_info,
				 int do_status,
				 struct processing_handle *handle)
{
	struct selection_handle *sh = handle->selection_handle;
	int ret = ECMD_PROCESSED;
	struct lv_segment *seg = pvseg->lvseg;

	struct segment_type _freeseg_type = {
		.name = "free",
		.flags = SEG_VIRTUAL | SEG_CANNOT_BE_ZEROED,
	};

	struct volume_group _free_vg = {
		.cmd = cmd,
		.name = "",
		.pvs = DM_LIST_HEAD_INIT(_free_vg.pvs),
		.lvs = DM_LIST_HEAD_INIT(_free_vg.lvs),
		.historical_lvs = DM_LIST_HEAD_INIT(_free_vg.historical_lvs),
		.tags = DM_LIST_HEAD_INIT(_free_vg.tags),
	};

	struct logical_volume _free_logical_volume = {
		.vg = vg ?: &_free_vg,
		.name = "",
		.status = VISIBLE_LV,
		.major = -1,
		.minor = -1,
		.tags = DM_LIST_HEAD_INIT(_free_logical_volume.tags),
		.segments = DM_LIST_HEAD_INIT(_free_logical_volume.segments),
		.segs_using_this_lv = DM_LIST_HEAD_INIT(_free_logical_volume.segs_using_this_lv),
		.indirect_glvs = DM_LIST_HEAD_INIT(_free_logical_volume.indirect_glvs),
		.snapshot_segs = DM_LIST_HEAD_INIT(_free_logical_volume.snapshot_segs),
	};

	struct lv_segment _free_lv_segment = {
		.lv = &_free_logical_volume,
		.segtype = &_freeseg_type,
		.len = pvseg->len,
		.tags = DM_LIST_HEAD_INIT(_free_lv_segment.tags),
		.origin_list = DM_LIST_HEAD_INIT(_free_lv_segment.origin_list),
	};

	struct lv_with_info_and_seg_status status = {
		.seg_status.type = SEG_STATUS_NONE,
		.lv = &_free_logical_volume
	};

	if (seg && !_do_info_and_status(cmd, seg->lv, seg, &status, do_info, do_status))
		goto_out;

	if (!report_object(sh ? : handle->custom_handle, sh != NULL,
			   vg, seg ? seg->lv : &_free_logical_volume,
			   pvseg->pv, seg ? : &_free_lv_segment, pvseg,
			   &status, pv_label(pvseg->pv))) {
		ret = ECMD_FAILED;
		goto_out;
	}

 out:
	if (status.seg_status.mem)
		dm_pool_destroy(status.seg_status.mem);

	return ret;
}

static int _pvsegs_sub_single(struct cmd_context *cmd,
			      struct volume_group *vg,
			      struct pv_segment *pvseg,
			      struct processing_handle *handle)
{
	return _do_pvsegs_sub_single(cmd, vg, pvseg, 0, 0, handle);
}

static int _pvsegs_with_lv_info_sub_single(struct cmd_context *cmd,
					   struct volume_group *vg,
					   struct pv_segment *pvseg,
					   struct processing_handle *handle)
{
	return _do_pvsegs_sub_single(cmd, vg, pvseg, 1, 0, handle);
}

static int _pvsegs_with_lv_status_sub_single(struct cmd_context *cmd,
					     struct volume_group *vg,
					     struct pv_segment *pvseg,
					     struct processing_handle *handle)
{
	return _do_pvsegs_sub_single(cmd, vg, pvseg, 0, 1, handle);
}

static int _pvsegs_with_lv_info_and_status_sub_single(struct cmd_context *cmd,
						      struct volume_group *vg,
						      struct pv_segment *pvseg,
						      struct processing_handle *handle)
{
	return _do_pvsegs_sub_single(cmd, vg, pvseg, 1, 1, handle);
}

static int _pvsegs_single(struct cmd_context *cmd,
			  struct volume_group *vg,
			  struct physical_volume *pv,
			  struct processing_handle *handle)
{
	return process_each_segment_in_pv(cmd, vg, pv, handle, _pvsegs_sub_single);
}

static int _pvsegs_with_lv_info_single(struct cmd_context *cmd,
				       struct volume_group *vg,
				       struct physical_volume *pv,
				       struct processing_handle *handle)
{
	return process_each_segment_in_pv(cmd, vg, pv, handle, _pvsegs_with_lv_info_sub_single);
}

static int _pvsegs_with_lv_status_single(struct cmd_context *cmd,
					 struct volume_group *vg,
					 struct physical_volume *pv,
					 struct processing_handle *handle)
{
	return process_each_segment_in_pv(cmd, vg, pv, handle, _pvsegs_with_lv_status_sub_single);
}

static int _pvsegs_with_lv_info_and_status_single(struct cmd_context *cmd,
						  struct volume_group *vg,
						  struct physical_volume *pv,
						  struct processing_handle *handle)
{
	return process_each_segment_in_pv(cmd, vg, pv, handle, _pvsegs_with_lv_info_and_status_sub_single);
}

static int _pvs_single(struct cmd_context *cmd, struct volume_group *vg,
		       struct physical_volume *pv,
		       struct processing_handle *handle)
{
	struct selection_handle *sh = handle->selection_handle;

	if (!report_object(sh ? : handle->custom_handle, sh != NULL,
			   vg, NULL, pv, NULL, NULL, NULL, NULL))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

static int _label_single(struct cmd_context *cmd, struct label *label,
		         struct processing_handle *handle)
{
	struct selection_handle *sh = handle->selection_handle;

	if (!report_object(sh ? : handle->custom_handle, sh != NULL,
			   NULL, NULL, NULL, NULL, NULL, NULL, label))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

static int _pvs_in_vg(struct cmd_context *cmd, const char *vg_name,
		      struct volume_group *vg,
		      struct processing_handle *handle)
{
	return process_each_pv_in_vg(cmd, vg, handle, &_pvs_single);
}

static int _pvsegs_in_vg(struct cmd_context *cmd, const char *vg_name,
			 struct volume_group *vg,
			 struct processing_handle *handle)
{
	return process_each_pv_in_vg(cmd, vg, handle, &_pvsegs_single);
}

static int _get_final_report_type(int args_are_pvs,
				  report_type_t report_type,
				  int *lv_info_needed,
				  int *lv_segment_status_needed,
				  report_type_t *final_report_type)
{
	/* Do we need to acquire LV device info in addition? */
	*lv_info_needed = (report_type & (LVSINFO | LVSINFOSTATUS)) ? 1 : 0;

	/* Do we need to acquire LV device status in addition? */
	*lv_segment_status_needed = (report_type & (LVSSTATUS | LVSINFOSTATUS)) ? 1 : 0;

	/* Ensure options selected are compatible */
	if (report_type & SEGS)
		report_type |= LVS;
	if (report_type & PVSEGS)
		report_type |= PVS;
	if ((report_type & (LVS | LVSINFO | LVSSTATUS | LVSINFOSTATUS)) &&
	    (report_type & (PVS | LABEL)) && !args_are_pvs) {
		log_error("Can't report LV and PV fields at the same time");
		return 0;
	}

	/* Change report type if fields specified makes this necessary */
	if (report_type & FULL)
		report_type = FULL;
	else if ((report_type & PVSEGS) ||
		 ((report_type & (PVS | LABEL)) && (report_type & (LVS | LVSINFO | LVSSTATUS | LVSINFOSTATUS))))
		report_type = PVSEGS;
	else if ((report_type & PVS) ||
		 ((report_type & LABEL) && (report_type & VGS)))
		report_type = PVS;
	else if (report_type & SEGS)
		report_type = SEGS;
	else if (report_type & (LVS | LVSINFO | LVSSTATUS | LVSINFOSTATUS))
		report_type = LVS;

	*final_report_type = report_type;
	return 1;
}

static int _report_all_in_vg(struct cmd_context *cmd, struct processing_handle *handle,
			     struct volume_group *vg, report_type_t type,
			     int do_lv_info, int do_lv_seg_status)
{
	int r = 0;

	switch (type) {
		case VGS:
			r = _vgs_single(cmd, vg->name, vg, handle);
			break;
		case LVS:
			r = process_each_lv_in_vg(cmd, vg, NULL, NULL, 0, handle,
						  do_lv_info && !do_lv_seg_status ? &_lvs_with_info_single :
						  !do_lv_info && do_lv_seg_status ? &_lvs_with_status_single :
						  do_lv_info && do_lv_seg_status ? &_lvs_with_info_and_status_single :
										   &_lvs_single);
			break;
		case SEGS:
			r = process_each_lv_in_vg(cmd, vg, NULL, NULL, 0, handle,
						  do_lv_info && !do_lv_seg_status ? &_lvsegs_with_info_single :
						  !do_lv_info && do_lv_seg_status ? &_lvsegs_with_status_single :
						  do_lv_info && do_lv_seg_status ? &_lvsegs_with_info_and_status_single :
										   &_lvsegs_single);
			break;
		case PVS:
			r = process_each_pv_in_vg(cmd, vg, handle, &_pvs_single);
			break;
		case PVSEGS:
			r = process_each_pv_in_vg(cmd, vg, handle,
						  do_lv_info && !do_lv_seg_status ? &_pvsegs_with_lv_info_single :
						  !do_lv_info && do_lv_seg_status ? &_pvsegs_with_lv_status_single :
						  do_lv_info && do_lv_seg_status ? &_pvsegs_with_lv_info_and_status_single :
										   &_pvsegs_single);
			break;
		default:
			log_error(INTERNAL_ERROR "_report_all_in_vg: incorrect report type");
			break;
	}

	return r;
}

static int _report_all_in_lv(struct cmd_context *cmd, struct processing_handle *handle,
			     struct logical_volume *lv, report_type_t type,
			     int do_lv_info, int do_lv_seg_status)
{
	int r = 0;

	switch (type) {
		case LVS:
			r = _do_lvs_with_info_and_status_single(cmd, lv, do_lv_info, do_lv_seg_status, handle);
			break;
		case SEGS:
			r = process_each_segment_in_lv(cmd, lv, handle,
						       do_lv_info && !do_lv_seg_status ? &_segs_with_info_single :
							       !do_lv_info && do_lv_seg_status ? &_segs_with_status_single :
							       do_lv_info && do_lv_seg_status ? &_segs_with_info_and_status_single :
												&_segs_single);
			break;
		default:
			log_error(INTERNAL_ERROR "_report_all_in_lv: incorrect report type");
			break;
	}

	return r;
}

static int _report_all_in_pv(struct cmd_context *cmd, struct processing_handle *handle,
			     struct physical_volume *pv, report_type_t type,
			     int do_lv_info, int do_lv_seg_status)
{
	int r = 0;

	switch (type) {
		case PVS:
			r = _pvs_single(cmd, pv->vg, pv, handle);
			break;
		case PVSEGS:
			r = process_each_segment_in_pv(cmd, pv->vg, pv, handle,
						       do_lv_info && !do_lv_seg_status ? &_pvsegs_with_lv_info_sub_single :
						       !do_lv_info && do_lv_seg_status ? &_pvsegs_with_lv_status_sub_single :
						       do_lv_info && do_lv_seg_status ? &_pvsegs_with_lv_info_and_status_sub_single :
											&_pvsegs_sub_single);
			break;
		default:
			log_error(INTERNAL_ERROR "_report_all_in_pv: incorrect report type");
			break;
	}

	return r;
}

int report_for_selection(struct cmd_context *cmd,
			 struct processing_handle *parent_handle,
			 struct physical_volume *pv,
			 struct volume_group *vg,
			 struct logical_volume *lv)
{
	struct selection_handle *sh = parent_handle->selection_handle;
	int args_are_pvs = sh->orig_report_type == PVS;
	int do_lv_info, do_lv_seg_status;
	struct processing_handle *handle;
	int r = 0;

	if (!_get_final_report_type(args_are_pvs,
				    sh->orig_report_type | sh->report_type,
				    &do_lv_info,
				    &do_lv_seg_status,
				    &sh->report_type))
		return_0;

	if (!(handle = init_processing_handle(cmd, parent_handle)))
		return_0;

	/*
	 * We're already reporting for select so override
	 * internal_report_for_select to 0 as we can call
	 * process_each_* functions again and we could
	 * end up in an infinite loop if we didn't stop
	 * internal reporting for select right here.
	 *
	 * So the overall call trace from top to bottom looks like this:
	 *
	 * process_each_* (top-level one, using processing_handle with internal reporting enabled and selection_handle) ->
	 *   select_match_*(processing_handle with selection_handle) ->
	 *     report for selection ->
	 *     	 (creating new processing_handle here with internal reporting disabled!!!)
	 *       reporting_fn OR process_each_* (using *new* processing_handle with original selection_handle) 
	 *
	 * The selection_handle is still reused so we can track
	 * whether any of the items the top-level one is composed
	 * of are still selected or not unerneath. Do not destroy
	 * this selection handle - it needs to be passed to upper
	 * layers to check the overall selection status.
	 */
	handle->internal_report_for_select = 0;
	handle->selection_handle = sh;

	/*
	 * Remember:
	 *   sh->orig_report_type is the original report type requested (what are we selecting? PV/VG/LV?)
	 *   sh->report_type is the report type actually used (it counts with all types of fields used in selection criteria)
	 */
	switch (sh->orig_report_type) {
		case LVS:
			r = _report_all_in_lv(cmd, handle, lv, sh->report_type, do_lv_info, do_lv_seg_status);
			break;
		case VGS:
			r = _report_all_in_vg(cmd, handle, vg, sh->report_type, do_lv_info, do_lv_seg_status);
			break;
		case PVS:
			r = _report_all_in_pv(cmd, handle, pv, sh->report_type, do_lv_info, do_lv_seg_status);
			break;
		default:
			log_error(INTERNAL_ERROR "report_for_selection: incorrect report type");
			break;
	}

	/*
	 * Keep the selection handle provided from the caller -
	 * do not destroy it - the caller will still use it to
	 * pass the result through it to layers above.
	 */
	handle->selection_handle = NULL;
	destroy_processing_handle(cmd, handle);
	return r;
}

static void _check_pv_list(struct cmd_context *cmd, struct report_args *args, struct single_report_args *single_args)
{
	unsigned i;
	int rescan_done = 0;

	if (!args->argv)
		return;

	single_args->args_are_pvs = (single_args->report_type == PVS ||
				     single_args->report_type == LABEL ||
				     single_args->report_type == PVSEGS) ? 1 : 0;

	if (single_args->args_are_pvs && args->argc) {
		for (i = 0; i < args->argc; i++) {
			if (!rescan_done && !dev_cache_get(args->argv[i], cmd->full_filter)) {
				cmd->filter->wipe(cmd->filter);
				/* FIXME scan only one device */
				lvmcache_label_scan(cmd);
				rescan_done = 1;
			}
			if (*args->argv[i] == '@') {
				/*
				 * Tags are metadata related, not label
				 * related, change report type accordingly!
				 */
				if (single_args->report_type == LABEL)
					single_args->report_type = PVS;
				/*
				 * If we changed the report_type and we did rescan,
				 * no need to iterate over dev list further - nothing
				 * else would change.
				 */
				if (rescan_done)
					break;
			}
		}
	}
}

static void _del_option_from_list(struct dm_list *sll, const char *prefix,
				  size_t prefix_len, const char *str)
{
	struct dm_list *slh;
	struct dm_str_list *sl;
	const char *a = str, *b;

	dm_list_uniterate(slh, sll, sll) {
		sl = dm_list_item(slh, struct dm_str_list);

		/* exact match */
		if (!strcmp(str, sl->str)) {
			dm_list_del(slh);
			return;
		}

		/* also try to match with known prefix */
		b = sl->str;
		if (!strncmp(prefix, a, prefix_len)) {
			a += prefix_len;
			if (*a == '_')
				a++;
		}
		if (!strncmp(prefix, b, prefix_len)) {
			b += prefix_len;
			if (*b == '_')
				b++;
		}
		if (!strcmp(a, b)) {
			dm_list_del(slh);
			return;
		}
	}
}

static report_idx_t _get_report_idx_from_name(const char *name, size_t len)
{
	report_idx_t idx = REPORT_IDX_NULL;

	if (!strncasecmp(name, "status", len))
		idx = REPORT_IDX_STATUS;
	else if (!strncasecmp(name, "vg", len))
		idx = REPORT_IDX_FULL_VGS;
	else if (!strncasecmp(name, "pv", len))
		idx = REPORT_IDX_FULL_PVS;
	else if (!strncasecmp(name, "lv", len))
		idx = REPORT_IDX_FULL_LVS;
	else if (!strncasecmp(name, "pvseg", len))
		idx = REPORT_IDX_FULL_PVSEGS;
	else if (!strncasecmp(name, "seg", len))
		idx = REPORT_IDX_FULL_SEGS;
	else
		log_error("Unknonwn report specifier in "
			  "report option list: %s.", name);

	return idx;
}

static const char *_get_report_idx(const char *opts, report_idx_t *idx)
{
	char *p;

	if (*opts != '/') {
		*idx = REPORT_IDX_SINGLE;
		return opts;
	}

	opts++;
	if (!(p = strchr(opts, '/'))) {
		log_error("Unfinished report specifier in "
			  "report options list: %s.", opts);
		*idx = -1;
		return NULL;
	}

	if ((*idx = _get_report_idx_from_name(opts, p - opts)) == -1)
		return_NULL;

	return p + 1;
}

static int _should_process_report_idx(report_type_t report_type, report_idx_t idx)
{
	if (((idx == REPORT_IDX_STATUS) && (report_type != CMDSTATUS)) ||
	    ((idx == REPORT_IDX_SINGLE) && ((report_type == FULL) || (report_type == CMDSTATUS))) ||
	    ((idx > REPORT_IDX_STATUS) && report_type != FULL))
		return 0;

	return 1;
}

enum opts_list_type {
	OPTS_REPLACE,
	OPTS_ADD,
	OPTS_REMOVE,
	OPTS_COMPACT
};

static int _get_report_options(struct cmd_context *cmd,
			       struct report_args *args,
			       struct single_report_args *single_args)
{
	int action;
	struct arg_value_group_list *current_group;
	struct dm_list *final_opts_list[REPORT_IDX_COUNT];
	struct dm_list *opts_list = NULL;
	struct dm_str_list *sl;
	struct dm_pool *mem;
	const char *opts;
	report_idx_t idx;
	int r = ECMD_FAILED;

	if (!arg_count(cmd, options_ARG))
		return ECMD_PROCESSED;

	if (!(mem = dm_pool_create("report_options", 128))) {
		log_error("Failed to create temporary mempool to process report options.");
		goto out;
	}

	if (single_args->report_type == CMDSTATUS) {
		if (!(final_opts_list[REPORT_IDX_STATUS] = str_to_str_list(mem, single_args->options, ",", 1)))
			goto_out;
	} else if (single_args->report_type == FULL) {
		if (!(final_opts_list[REPORT_IDX_FULL_VGS] = str_to_str_list(mem, args->single_args[REPORT_IDX_FULL_VGS].options, ",", 1)) ||
		    !(final_opts_list[REPORT_IDX_FULL_PVS] = str_to_str_list(mem, args->single_args[REPORT_IDX_FULL_PVS].options, ",", 1)) ||
		    !(final_opts_list[REPORT_IDX_FULL_LVS] = str_to_str_list(mem, args->single_args[REPORT_IDX_FULL_LVS].options, ",", 1)) ||
		    !(final_opts_list[REPORT_IDX_FULL_PVSEGS] = str_to_str_list(mem, args->single_args[REPORT_IDX_FULL_PVSEGS].options, ",", 1)) ||
		    !(final_opts_list[REPORT_IDX_FULL_SEGS] = str_to_str_list(mem, args->single_args[REPORT_IDX_FULL_SEGS].options, ",", 1)))
			goto_out;
	} else {
		if (!(final_opts_list[REPORT_IDX_SINGLE] = str_to_str_list(mem, single_args->options, ",", 1)))
			goto_out;
	}

	dm_list_iterate_items(current_group, &cmd->arg_value_groups) {
		if (!grouped_arg_is_set(current_group->arg_values, options_ARG))
			continue;

		opts = grouped_arg_str_value(current_group->arg_values, options_ARG, NULL);
		if (!opts || !*opts) {
			log_error("Invalid options string: %s", opts);
			r = EINVALID_CMD_LINE;
			goto out;
		}

		switch (*opts) {
			case '+':
				action = OPTS_ADD;
				opts++;
				break;
			case '-':
				action = OPTS_REMOVE;
				opts++;
				break;
			case '#':
				action = OPTS_COMPACT;
				opts++;
				break;
			default:
				action = OPTS_REPLACE;
		}

		if (!(opts = _get_report_idx(opts, &idx)))
			goto_out;

		if (!_should_process_report_idx(single_args->report_type, idx))
			continue;

		if ((action != OPTS_COMPACT) &&
		    !(opts_list = str_to_str_list(mem, opts, ",", 1)))
			goto_out;

		switch (action) {
			case OPTS_ADD:
				dm_list_splice(final_opts_list[idx], opts_list);
				break;
			case OPTS_REMOVE:
				dm_list_iterate_items(sl, opts_list)
					_del_option_from_list(final_opts_list[idx], args->single_args[idx].report_prefix,
							      strlen(args->single_args[idx].report_prefix), sl->str);
				break;
			case OPTS_COMPACT:
				args->single_args[idx].fields_to_compact = opts;
				break;
			case OPTS_REPLACE:
				final_opts_list[idx] = opts_list;
				break;
		}
	}

	if (single_args->report_type == CMDSTATUS) {
		if (!(single_args->options = str_list_to_str(cmd->mem, final_opts_list[REPORT_IDX_STATUS], ",")))
			goto_out;
	} else if (single_args->report_type == FULL) {
		if (!(args->single_args[REPORT_IDX_FULL_VGS].options = str_list_to_str(cmd->mem, final_opts_list[REPORT_IDX_FULL_VGS], ",")) ||
		    !(args->single_args[REPORT_IDX_FULL_PVS].options = str_list_to_str(cmd->mem, final_opts_list[REPORT_IDX_FULL_PVS], ",")) ||
		    !(args->single_args[REPORT_IDX_FULL_LVS].options = str_list_to_str(cmd->mem, final_opts_list[REPORT_IDX_FULL_LVS], ",")) ||
		    !(args->single_args[REPORT_IDX_FULL_PVSEGS].options = str_list_to_str(cmd->mem, final_opts_list[REPORT_IDX_FULL_PVSEGS], ",")) ||
		    !(args->single_args[REPORT_IDX_FULL_SEGS].options = str_list_to_str(cmd->mem, final_opts_list[REPORT_IDX_FULL_SEGS], ",")))
			goto_out;
	} else {
		if (!(single_args->options = str_list_to_str(cmd->mem, final_opts_list[REPORT_IDX_SINGLE], ",")))
			goto_out;
	}

	r = ECMD_PROCESSED;
out:
	dm_pool_destroy(mem);
	return r;
}

static int _get_report_keys(struct cmd_context *cmd,
			    struct report_args *args,
			    struct single_report_args *single_args)
{
	struct arg_value_group_list *current_group;
	const char *keys;
	report_idx_t idx;
	int r = ECMD_FAILED;

	dm_list_iterate_items(current_group, &cmd->arg_value_groups) {
		if (!grouped_arg_is_set(current_group->arg_values, sort_ARG))
			continue;

		keys = grouped_arg_str_value(current_group->arg_values, sort_ARG, NULL);
		if (!keys || !*keys) {
			log_error("Invalid keys string: %s", keys);
			r = EINVALID_CMD_LINE;
			goto out;
		}

		if (!(keys = _get_report_idx(keys, &idx)))
			goto_out;

		if (!_should_process_report_idx(single_args->report_type, idx))
			continue;

		args->single_args[idx].keys = keys;
	}

	r = ECMD_PROCESSED;
out:
	return r;
}

static int _get_report_selection(struct cmd_context *cmd,
				 struct report_args *args,
				 struct single_report_args *single_args)
{
	int r = ECMD_PROCESSED;

	if (arg_count(cmd, select_ARG))
		single_args->selection = arg_str_value(cmd, select_ARG, NULL);

	return r;
}

static int _get_report_prefix(report_type_t report_type, char *buf, size_t buf_len)
{
	const char *report_prefix;
	size_t len;

	if (report_type == FULL) {
		buf[0] = '\0';
		return 1;
	}

	report_prefix = report_get_field_prefix(report_type);
	len = strlen(report_prefix);
	if (report_prefix[len - 1] == '_')
		len--;

	if (!len) {
		log_error(INTERNAL_ERROR "_get_report_prefix: no prefix "
			  "found for report type 0x%x", report_type);
		return 0;
	}

	if (!dm_strncpy(buf, report_prefix, buf_len)) {
		log_error("_get_report_prefix: dm_strncpy failed");
		return 0;
	}
	buf[len] = '\0';

	return 1;
}

static int _do_report(struct cmd_context *cmd, struct processing_handle *handle,
		      struct report_args *args, struct single_report_args *single_args)
{
	void *orig_custom_handle = handle->custom_handle;
	report_type_t report_type = single_args->report_type;
	void *report_handle = NULL;
	int lock_global = 0;
	int lv_info_needed;
	int lv_segment_status_needed;
	int report_in_group = 0;
	int r = ECMD_FAILED;

	if (!(report_handle = report_init(cmd, single_args->options, single_args->keys, &report_type,
					  args->separator, args->aligned, args->buffered,
					  args->headings, args->field_prefixes, args->quoted,
					  args->columns_as_rows, single_args->selection)))
		goto_out;

	handle->custom_handle = report_handle;

	if (!_get_final_report_type(single_args->args_are_pvs,
				    report_type, &lv_info_needed,
				    &lv_segment_status_needed,
				    &report_type))
		goto_out;

	if (!dm_report_group_push(handle->report_group, report_handle, (void *) single_args->report_prefix))
		goto_out;
	report_in_group = 1;

	/*
	 * We lock VG_GLOBAL to enable use of metadata cache.
	 * This can pause alongide pvscan or vgscan process for a while.
	 */
	if (single_args->args_are_pvs && (report_type == PVS || report_type == PVSEGS) &&
	    !lvmetad_used()) {
		lock_global = 1;
		if (!lock_vol(cmd, VG_GLOBAL, LCK_VG_READ, NULL)) {
			log_error("Unable to obtain global lock.");
			goto out;
		}
	}

	switch (report_type) {
		case DEVTYPES:
			r = _process_each_devtype(cmd, args->argc, handle);
			break;
		case LVSINFO:
			/* fall through */
		case LVSSTATUS:
			/* fall through */
		case LVSINFOSTATUS:
			/* fall through */
		case LVS:
			if (args->full_report_vg)
				r = _report_all_in_vg(cmd, handle, args->full_report_vg, LVS, lv_info_needed, lv_segment_status_needed);
			else
				r = process_each_lv(cmd, args->argc, args->argv, NULL, NULL, 0, handle,
						    lv_info_needed && !lv_segment_status_needed ? &_lvs_with_info_single :
						    !lv_info_needed && lv_segment_status_needed ? &_lvs_with_status_single :
						    lv_info_needed && lv_segment_status_needed ? &_lvs_with_info_and_status_single :
												 &_lvs_single);
			break;
		case VGS:
			if (args->full_report_vg)
				r = _report_all_in_vg(cmd, handle, args->full_report_vg, VGS, lv_info_needed, lv_segment_status_needed);
			else
				r = process_each_vg(cmd, args->argc, args->argv, NULL, NULL, 0,
						    handle, &_vgs_single);
			break;
		case LABEL:
			r = process_each_label(cmd, args->argc, args->argv,
					       handle, &_label_single);
			break;
		case PVS:
			if (args->full_report_vg)
				r = _report_all_in_vg(cmd, handle, args->full_report_vg, PVS, lv_info_needed, lv_segment_status_needed);
			else {
				if (single_args->args_are_pvs)
					r = process_each_pv(cmd, args->argc, args->argv, NULL,
							    arg_is_set(cmd, all_ARG), 0,
							    handle, &_pvs_single);
				else
					r = process_each_vg(cmd, args->argc, args->argv, NULL, NULL,
							    0, handle, &_pvs_in_vg);
			}
			break;
		case SEGS:
			if (args->full_report_vg)
				r = _report_all_in_vg(cmd, handle, args->full_report_vg, SEGS, lv_info_needed, lv_segment_status_needed);
			else
				r = process_each_lv(cmd, args->argc, args->argv, NULL, NULL, 0, handle,
						    lv_info_needed && !lv_segment_status_needed ? &_lvsegs_with_info_single :
						    !lv_info_needed && lv_segment_status_needed ? &_lvsegs_with_status_single :
						    lv_info_needed && lv_segment_status_needed ? &_lvsegs_with_info_and_status_single :
												 &_lvsegs_single);
			break;
		case PVSEGS:
			if (args->full_report_vg)
				r = _report_all_in_vg(cmd, handle, args->full_report_vg, PVSEGS, lv_info_needed, lv_segment_status_needed);
			else {
				if (single_args->args_are_pvs)
					r = process_each_pv(cmd, args->argc, args->argv, NULL,
							    arg_is_set(cmd, all_ARG), 0,
							    handle,
							    lv_info_needed && !lv_segment_status_needed ? &_pvsegs_with_lv_info_single :
							    !lv_info_needed && lv_segment_status_needed ? &_pvsegs_with_lv_status_single :
							    lv_info_needed && lv_segment_status_needed ? &_pvsegs_with_lv_info_and_status_single :
												 &_pvsegs_single);
				else
					r = process_each_vg(cmd, args->argc, args->argv, NULL, NULL,
							    0, handle, &_pvsegs_in_vg);
			}
			break;
		case FULL:
			/*
			 * Full report's subreports already covered by combinations above with args->full_report_vg.
			 * We shouldn't see report_type == FULL in this function.
			 */
			log_error(INTERNAL_ERROR "_do_report: full report requested at incorrect level");
			break;
		case CMDSTATUS:
			/* Status is reported throughout the code via report_cmdstatus calls. */
			break;
		default:
			log_error(INTERNAL_ERROR "_do_report: unknown report type.");
			return 0;
	}

	if (find_config_tree_bool(cmd, report_compact_output_CFG, NULL)) {
		if (!dm_report_compact_fields(report_handle))
			log_error("Failed to compact report output.");
	} else if (single_args->fields_to_compact) {
		if (!dm_report_compact_given_fields(report_handle, single_args->fields_to_compact))
			log_error("Failed to compact given columns in report output.");
	}

	dm_report_output(report_handle);

	if (lock_global)
		unlock_vg(cmd, VG_GLOBAL);
out:
	if (report_handle) {
		if (report_in_group && !dm_report_group_pop(handle->report_group))
			stack;
		dm_report_free(report_handle);
	}

	handle->custom_handle = orig_custom_handle;
	return r;
}

static int _full_report_single(struct cmd_context *cmd,
			       const char *vg_name,
			       struct volume_group *vg,
			       struct processing_handle *handle)
{
	struct report_args *args = (struct report_args *) handle->custom_handle;
	int orphan = is_orphan_vg(vg->name);
	int r = ECMD_FAILED;

	if (orphan && !dm_list_size(&vg->pvs))
		return ECMD_PROCESSED;

	args->full_report_vg = vg;

	if (!dm_report_group_push(handle->report_group, NULL, NULL))
		goto out;

	if (orphan) {
		if (((r = _do_report(cmd, handle, args, &args->single_args[REPORT_IDX_FULL_PVS])) != ECMD_PROCESSED) ||
		    ((r = _do_report(cmd, handle, args, &args->single_args[REPORT_IDX_FULL_PVSEGS])) != ECMD_PROCESSED))
			stack;
	} else {
		if (((r = _do_report(cmd, handle, args, &args->single_args[REPORT_IDX_FULL_VGS])) != ECMD_PROCESSED) ||
		    ((r = _do_report(cmd, handle, args, &args->single_args[REPORT_IDX_FULL_PVS])) != ECMD_PROCESSED) ||
		    ((r = _do_report(cmd, handle, args, &args->single_args[REPORT_IDX_FULL_LVS])) != ECMD_PROCESSED) ||
		    ((r = _do_report(cmd, handle, args, &args->single_args[REPORT_IDX_FULL_PVSEGS])) != ECMD_PROCESSED) ||
		    ((r = _do_report(cmd, handle, args, &args->single_args[REPORT_IDX_FULL_SEGS])) != ECMD_PROCESSED))
			stack;
	}

	if (!dm_report_group_pop(handle->report_group))
		stack;
out:
	args->full_report_vg = NULL;
	return r;
}

#define set_full_report_single(ptr,type,name) \
	do { \
		(ptr)->report_type = type; \
		(ptr)->keys = find_config_tree_str(cmd, report_ ## name ## _sort_full_CFG, NULL); \
		(ptr)->options = find_config_tree_str(cmd, report_ ## name ## _cols_full_CFG, NULL); \
		if (!_get_report_prefix(type, (ptr)->report_prefix, sizeof((ptr)->report_prefix))) \
			return_0; \
	} while (0)

static int _config_report(struct cmd_context *cmd, struct report_args *args, struct single_report_args *single_args)
{
	args->aligned = find_config_tree_bool(cmd, report_aligned_CFG, NULL);
	args->buffered = find_config_tree_bool(cmd, report_buffered_CFG, NULL);
	args->headings = find_config_tree_bool(cmd, report_headings_CFG, NULL);
	args->separator = find_config_tree_str(cmd, report_separator_CFG, NULL);
	args->field_prefixes = find_config_tree_bool(cmd, report_prefixes_CFG, NULL);
	args->quoted = find_config_tree_bool(cmd, report_quoted_CFG, NULL);
	args->columns_as_rows = find_config_tree_bool(cmd, report_colums_as_rows_CFG, NULL);

	/* Check PV specifics and do extra changes/actions if needed. */
	_check_pv_list(cmd, args, single_args);

	if (!_get_report_prefix(single_args->report_type, single_args->report_prefix, sizeof(single_args->report_prefix)))
		return_0;

	switch (single_args->report_type) {
		case DEVTYPES:
			single_args->keys = find_config_tree_str(cmd, report_devtypes_sort_CFG, NULL);
			if (!arg_count(cmd, verbose_ARG))
				single_args->options = find_config_tree_str(cmd, report_devtypes_cols_CFG, NULL);
			else
				single_args->options = find_config_tree_str(cmd, report_devtypes_cols_verbose_CFG, NULL);
			break;
		case LVS:
			single_args->keys = find_config_tree_str(cmd, report_lvs_sort_CFG, NULL);
			if (!arg_count(cmd, verbose_ARG))
				single_args->options = find_config_tree_str(cmd, report_lvs_cols_CFG, NULL);
			else
				single_args->options = find_config_tree_str(cmd, report_lvs_cols_verbose_CFG, NULL);
			break;
		case VGS:
			single_args->keys = find_config_tree_str(cmd, report_vgs_sort_CFG, NULL);
			if (!arg_count(cmd, verbose_ARG))
				single_args->options = find_config_tree_str(cmd, report_vgs_cols_CFG, NULL);
			else
				single_args->options = find_config_tree_str(cmd, report_vgs_cols_verbose_CFG, NULL);
			break;
		case LABEL:
		case PVS:
			single_args->keys = find_config_tree_str(cmd, report_pvs_sort_CFG, NULL);
			if (!arg_count(cmd, verbose_ARG))
				single_args->options = find_config_tree_str(cmd, report_pvs_cols_CFG, NULL);
			else
				single_args->options = find_config_tree_str(cmd, report_pvs_cols_verbose_CFG, NULL);
			break;
		case SEGS:
			single_args->keys = find_config_tree_str(cmd, report_segs_sort_CFG, NULL);
			if (!arg_count(cmd, verbose_ARG))
				single_args->options = find_config_tree_str(cmd, report_segs_cols_CFG, NULL);
			else
				single_args->options = find_config_tree_str(cmd, report_segs_cols_verbose_CFG, NULL);
			break;
		case PVSEGS:
			single_args->keys = find_config_tree_str(cmd, report_pvsegs_sort_CFG, NULL);
			if (!arg_count(cmd, verbose_ARG))
				single_args->options = find_config_tree_str(cmd, report_pvsegs_cols_CFG, NULL);
			else
				single_args->options = find_config_tree_str(cmd, report_pvsegs_cols_verbose_CFG, NULL);
			break;
		case FULL:
			set_full_report_single(&args->single_args[REPORT_IDX_FULL_VGS], VGS, vgs);
			set_full_report_single(&args->single_args[REPORT_IDX_FULL_LVS], LVS, lvs);
			set_full_report_single(&args->single_args[REPORT_IDX_FULL_PVS], PVS, pvs);
			set_full_report_single(&args->single_args[REPORT_IDX_FULL_PVSEGS], PVSEGS, pvsegs);
			set_full_report_single(&args->single_args[REPORT_IDX_FULL_SEGS], SEGS, segs);
			break;
		case CMDSTATUS:
			single_args->keys = find_config_tree_str(cmd, report_status_sort_CFG, NULL);
			single_args->options = find_config_tree_str(cmd, report_status_cols_CFG, NULL);
			break;
		default:
			log_error(INTERNAL_ERROR "_report: unknown report type.");
			return 0;
	}

	if (single_args->report_type != FULL)
		single_args->fields_to_compact = find_config_tree_str_allow_empty(cmd, report_compact_output_cols_CFG, NULL);

	/* If -o supplied use it, else use default for report_type */
	if ((_get_report_options(cmd, args, single_args) != ECMD_PROCESSED))
		return_0;

	/* -O overrides default sort settings */
	if ((_get_report_keys(cmd, args, single_args) != ECMD_PROCESSED))
		return_0;

	if ((_get_report_selection(cmd, args, single_args) != ECMD_PROCESSED))
		return_0;

	args->separator = arg_str_value(cmd, separator_ARG, args->separator);
	if (arg_count(cmd, separator_ARG))
		args->aligned = 0;
	if (arg_count(cmd, aligned_ARG))
		args->aligned = 1;
	if (arg_count(cmd, unbuffered_ARG) && !arg_count(cmd, sort_ARG))
		args->buffered = 0;
	if (arg_count(cmd, noheadings_ARG))
		args->headings = 0;
	if (arg_count(cmd, nameprefixes_ARG)) {
		args->aligned = 0;
		args->field_prefixes = 1;
	}
	if (arg_count(cmd, unquoted_ARG))
		args->quoted = 0;
	if (arg_count(cmd, rows_ARG))
		args->columns_as_rows = 1;

	return 1;
}

static int _report(struct cmd_context *cmd, int argc, char **argv, report_type_t report_type)
{
	struct report_args args = {0};
	struct single_report_args *single_args = &args.single_args[REPORT_IDX_SINGLE];
	static char report_name[] = "report";
	struct processing_handle *handle;
	int r = ECMD_FAILED;

	/*
	 * Include foreign VGs that contain active LVs.
	 * That shouldn't happen in general, but if it does by some
	 * mistake, then we want to display those VGs and allow the
	 * LVs to be deactivated.
	 */
	cmd->include_active_foreign_vgs = 1;

	args.argc = argc;
	args.argv = argv;
	single_args->report_type = report_type;

	if (!_config_report(cmd, &args, single_args))
		return_ECMD_FAILED;

	if (!(handle = init_processing_handle(cmd, NULL)))
		return_ECMD_FAILED;

	handle->internal_report_for_select = 0;
	handle->include_historical_lvs = cmd->include_historical_lvs;

	if (!dm_report_group_push(handle->report_group, NULL, report_name)) {
		log_error("Failed to add main report section to report group.");
		destroy_processing_handle(cmd, handle);
		return ECMD_FAILED;
	}

	if (single_args->report_type == FULL) {
		handle->custom_handle = &args;
		r = process_each_vg(cmd, argc, argv, NULL, NULL, 0, handle, &_full_report_single);
	} else
		r = _do_report(cmd, handle, &args, single_args);

	dm_report_group_pop(handle->report_group);
	destroy_processing_handle(cmd, handle);
	return r;
}

int lvs(struct cmd_context *cmd, int argc, char **argv)
{
	report_type_t type;

	if (arg_count(cmd, segments_ARG))
		type = SEGS;
	else
		type = LVS;

	return _report(cmd, argc, argv, type);
}

int vgs(struct cmd_context *cmd, int argc, char **argv)
{
	return _report(cmd, argc, argv, VGS);
}

int pvs(struct cmd_context *cmd, int argc, char **argv)
{
	report_type_t type;

	if (arg_count(cmd, segments_ARG))
		type = PVSEGS;
	else
		type = LABEL;

	return _report(cmd, argc, argv, type);
}

int fullreport(struct cmd_context *cmd, int argc, char **argv)
{
	return _report(cmd, argc, argv, FULL);
}

int devtypes(struct cmd_context *cmd, int argc, char **argv)
{
	return _report(cmd, argc, argv, DEVTYPES);
}

#define REPORT_FORMAT_NAME_NATIVE "native"
#define REPORT_FORMAT_NAME_EXTENDED "extended"
#define REPORT_FORMAT_NAME_JSON "json"

int report_format_init(struct cmd_context *cmd, struct dm_report_group **report_group,
		       struct dm_report **status_rh)
{
	static char status_report_name[] = "status";
	struct report_args args = {0};
	struct single_report_args *single_args;
	const char *format_str;
	dm_report_group_type_t group_type;
	struct dm_report_group *new_report_group;
	struct dm_report *tmp_status_rh = NULL;

	if (!(format_str = arg_str_value(cmd, reportformat_ARG, find_config_tree_str(cmd, report_output_format_CFG, NULL))))
		return_0;

	if (!strcmp(format_str, REPORT_FORMAT_NAME_NATIVE))
		return 1;

	if (!strcmp(format_str, REPORT_FORMAT_NAME_EXTENDED))
		group_type = DM_REPORT_GROUP_EXTENDED;
	else if (!strcmp(format_str, REPORT_FORMAT_NAME_JSON))
		group_type = DM_REPORT_GROUP_JSON;
	else {
		log_error("%s: unknown report format.", format_str);
		log_error("Supported report formats: %s, %s, %s.",
			  REPORT_FORMAT_NAME_NATIVE,
			  REPORT_FORMAT_NAME_EXTENDED,
			  REPORT_FORMAT_NAME_JSON);
		return 0;
	}

	if (!(new_report_group = dm_report_group_create(group_type, NULL))) {
		log_error("Failed to create report group.");
		return 0;
	}

	if (!*status_rh) {
		single_args = &args.single_args[REPORT_IDX_STATUS];
		single_args->report_type = CMDSTATUS;

		if (!_config_report(cmd, &args, single_args))
			goto_bad;

		if (!(tmp_status_rh = report_init(NULL, single_args->options, single_args->keys, &single_args->report_type,
						  args.separator, args.aligned, args.buffered, args.headings,
						  args.field_prefixes, args.quoted, args.columns_as_rows,
						  single_args->selection))) {
			log_error("Failed to create status report.");
			goto bad;
		}
	}

	if (!(dm_report_group_push(new_report_group, *status_rh ? : tmp_status_rh, status_report_name))) {
		log_error("Failed to add status report to report group.");
		goto bad;
	}

	*report_group = new_report_group;
	if (tmp_status_rh)
		*status_rh = tmp_status_rh;
	return 1;
bad:
	if (!dm_report_group_destroy(new_report_group))
		stack;
	if (tmp_status_rh)
		dm_report_free(tmp_status_rh);
	return 0;
}
