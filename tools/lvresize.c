/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2013 Red Hat, Inc. All rights reserved.
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

#include "tools.h"

#define SIZE_BUF 128

struct lvresize_params {
	const char *vg_name;
	const char *lv_name;

	uint32_t stripes;
	uint32_t stripe_size;
	uint32_t mirrors;

	const struct segment_type *segtype;

	/* size */
	uint32_t extents;
	uint64_t size;
	int sizeargs;
	sign_t sign;
	uint64_t poolmetadatasize;
	sign_t poolmetadatasign;
	percent_type_t percent;

	enum {
		LV_ANY = 0,
		LV_REDUCE = 1,
		LV_EXTEND = 2
	} resize;

	int resizefs;
	int nofsck;

	int argc;
	char **argv;

	/* Arg counts & values */
	unsigned ac_policy;
	unsigned ac_stripes;
	uint32_t ac_stripes_value;
	unsigned ac_mirrors;
	uint32_t ac_mirrors_value;
	unsigned ac_stripesize;
	uint64_t ac_stripesize_value;
	unsigned ac_alloc;
	unsigned ac_no_sync;
	unsigned ac_force;

	const char *ac_type;
};

static int _validate_stripesize(struct cmd_context *cmd,
				const struct volume_group *vg,
				struct lvresize_params *lp)
{

	if ( lp->ac_stripesize_value > STRIPE_SIZE_LIMIT * 2) {
		log_error("Stripe size cannot be larger than %s",
			  display_size(cmd, (uint64_t) STRIPE_SIZE_LIMIT));
		return 0;
	}

	if (!(vg->fid->fmt->features & FMT_SEGMENTS))
		log_warn("Varied stripesize not supported. Ignoring.");
	else if (lp->ac_stripesize_value > (uint64_t) vg->extent_size * 2) {
		log_error("Reducing stripe size %s to maximum, "
			  "physical extent size %s",
			  display_size(cmd,lp->ac_stripesize_value),
			  display_size(cmd, (uint64_t) vg->extent_size));
		lp->stripe_size = vg->extent_size;
	} else
		lp->stripe_size = lp->ac_stripesize_value;

	if (lp->stripe_size & (lp->stripe_size - 1)) {
		log_error("Stripe size must be power of 2");
		return 0;
	}

	return 1;
}

static int _request_confirmation(struct cmd_context *cmd,
				 const struct volume_group *vg,
				 const struct logical_volume *lv,
				 const struct lvresize_params *lp)
{
	struct lvinfo info = { 0 };

	if (!lv_info(cmd, lv, 0, &info, 1, 0) && driver_version(NULL, 0)) {
		log_error("lv_info failed: aborting");
		return 0;
	}

	if (lp->resizefs) {
		if (!info.exists) {
			log_error("Logical volume %s must be activated "
				  "before resizing filesystem", lp->lv_name);
			return 0;
		}
		return 1;
	}

	if (!info.exists)
		return 1;

	log_warn("WARNING: Reducing active%s logical volume to %s",
		 info.open_count ? " and open" : "",
		 display_size(cmd, (uint64_t) lp->extents * vg->extent_size));

	log_warn("THIS MAY DESTROY YOUR DATA (filesystem etc.)");

	if (!lp->ac_force) {
		if (yes_no_prompt("Do you really want to reduce %s? [y/n]: ",
				  lp->lv_name) == 'n') {
			log_error("Logical volume %s NOT reduced", lp->lv_name);
			return 0;
		}
		if (sigint_caught())
			return 0;
	}

	return 1;
}

enum fsadm_cmd_e { FSADM_CMD_CHECK, FSADM_CMD_RESIZE };
#define FSADM_CMD "fsadm"
#define FSADM_CMD_MAX_ARGS 6
#define FSADM_CHECK_FAILS_FOR_MOUNTED 3 /* shell exist status code */

/*
 * FSADM_CMD --dry-run --verbose --force check lv_path
 * FSADM_CMD --dry-run --verbose --force resize lv_path size
 */
static int _fsadm_cmd(struct cmd_context *cmd,
		      const struct volume_group *vg,
		      const struct lvresize_params *lp,
		      enum fsadm_cmd_e fcmd,
		      int *status)
{
	char lv_path[PATH_MAX];
	char size_buf[SIZE_BUF];
	const char *argv[FSADM_CMD_MAX_ARGS + 2];
	unsigned i = 0;

	argv[i++] = FSADM_CMD;

	if (test_mode())
		argv[i++] = "--dry-run";

	if (verbose_level() >= _LOG_NOTICE)
		argv[i++] = "--verbose";

	if (lp->ac_force)
		argv[i++] = "--force";

	argv[i++] = (fcmd == FSADM_CMD_RESIZE) ? "resize" : "check";

	if (status)
		*status = -1;

	if (dm_snprintf(lv_path, PATH_MAX, "%s%s/%s", cmd->dev_dir, lp->vg_name,
			lp->lv_name) < 0) {
		log_error("Couldn't create LV path for %s", lp->lv_name);
		return 0;
	}

	argv[i++] = lv_path;

	if (fcmd == FSADM_CMD_RESIZE) {
		if (dm_snprintf(size_buf, SIZE_BUF, "%" PRIu64 "K",
				(uint64_t) lp->extents * vg->extent_size / 2) < 0) {
			log_error("Couldn't generate new LV size string");
			return 0;
		}

		argv[i++] = size_buf;
	}

	argv[i] = NULL;

	return exec_cmd(cmd, argv, status, 1);
}

static int _lvresize_params(struct cmd_context *cmd, int argc, char **argv,
			    struct lvresize_params *lp)
{
	const char *cmd_name;
	char *st;
	unsigned dev_dir_found = 0;
	int use_policy = arg_count(cmd, use_policies_ARG);

	lp->sign = SIGN_NONE;
	lp->poolmetadatasign = SIGN_NONE;
	lp->resize = LV_ANY;

	cmd_name = command_name(cmd);
	if (!strcmp(cmd_name, "lvreduce"))
		lp->resize = LV_REDUCE;
	if (!strcmp(cmd_name, "lvextend"))
		lp->resize = LV_EXTEND;

	if (use_policy) {
		/* do nothing; _lvresize will handle --use-policies itself */
		lp->extents = 0;
		lp->sign = SIGN_PLUS;
		lp->percent = PERCENT_LV;
	} else {
		/*
		 * Allow omission of extents and size if the user has given us
		 * one or more PVs.  Most likely, the intent was "resize this
		 * LV the best you can with these PVs"
		 * If only --poolmetadatasize is specified with list of PVs,
		 * then metadata will be extended there.
		 */
		lp->sizeargs = arg_count(cmd, extents_ARG) + arg_count(cmd, size_ARG);
		if ((lp->sizeargs == 0) && (argc >= 2)) {
			lp->extents = 100;
			lp->percent = PERCENT_PVS;
			lp->sign = SIGN_PLUS;
			lp->sizeargs = !lp->poolmetadatasize ? 1 : 0;
		} else if ((lp->sizeargs != 1) &&
			   ((lp->sizeargs == 2) ||
			    !arg_count(cmd, poolmetadatasize_ARG))) {
			log_error("Please specify either size or extents but not "
				  "both.");
			return 0;
		}

		if (arg_count(cmd, extents_ARG)) {
			lp->extents = arg_uint_value(cmd, extents_ARG, 0);
			lp->sign = arg_sign_value(cmd, extents_ARG, SIGN_NONE);
			lp->percent = arg_percent_value(cmd, extents_ARG, PERCENT_NONE);
		}

		/* Size returned in kilobyte units; held in sectors */
		if (arg_count(cmd, size_ARG)) {
			lp->size = arg_uint64_value(cmd, size_ARG, 0);
			lp->sign = arg_sign_value(cmd, size_ARG, SIGN_NONE);
			lp->percent = PERCENT_NONE;
		}

		if (arg_count(cmd, poolmetadatasize_ARG)) {
			lp->poolmetadatasize = arg_uint64_value(cmd, poolmetadatasize_ARG, 0);
			lp->poolmetadatasign = arg_sign_value(cmd, poolmetadatasize_ARG, SIGN_NONE);
			if (lp->poolmetadatasign == SIGN_MINUS) {
				log_error("Can't reduce pool metadata size.");
				return 0;
			}
		}
	}

	if (lp->resize == LV_EXTEND && lp->sign == SIGN_MINUS) {
		log_error("Negative argument not permitted - use lvreduce");
		return 0;
	}

	if (lp->resize == LV_REDUCE &&
	    ((lp->sign == SIGN_PLUS) || (lp->poolmetadatasign == SIGN_PLUS))) {
		log_error("Positive sign not permitted - use lvextend");
		return 0;
	}

	lp->resizefs = arg_is_set(cmd, resizefs_ARG);
	lp->nofsck = arg_is_set(cmd, nofsck_ARG);

	if (!argc) {
		log_error("Please provide the logical volume name");
		return 0;
	}

	lp->lv_name = argv[0];
	argv++;
	argc--;

	if (!(lp->lv_name = skip_dev_dir(cmd, lp->lv_name, &dev_dir_found)) ||
	    !(lp->vg_name = extract_vgname(cmd, lp->lv_name))) {
		log_error("Please provide a volume group name");
		return 0;
	}

	if (!validate_name(lp->vg_name)) {
		log_error("Volume group name %s has invalid characters",
			  lp->vg_name);
		return 0;
	}

	if ((st = strrchr(lp->lv_name, '/')))
		lp->lv_name = st + 1;

	lp->argc = argc;
	lp->argv = argv;

	lp->ac_policy = arg_count(cmd, use_policies_ARG);
	lp->ac_stripes = arg_count(cmd, stripes_ARG);
	if (lp->ac_stripes) {
		lp->ac_stripes_value = arg_uint_value(cmd, stripes_ARG, 1);
	} else {
		lp->ac_stripes_value = 0;
	}

	lp->ac_mirrors = arg_count(cmd, mirrors_ARG);

	if (lp->ac_mirrors) {
		if (arg_sign_value(cmd, mirrors_ARG, SIGN_NONE) == SIGN_MINUS) {
			log_error("Mirrors argument may not be negative");
			return 0;
		}

		lp->ac_mirrors_value = arg_uint_value(cmd, mirrors_ARG, 1) + 1;
	} else {
		lp->ac_mirrors_value = 0;
	}

	lp->ac_stripesize = arg_count(cmd, stripesize_ARG);
	if (lp->ac_stripesize) {
		if (arg_sign_value(cmd, stripesize_ARG, SIGN_NONE) == SIGN_MINUS) {
			log_error("Stripesize may not be negative.");
			return 0;
		}

		lp->ac_stripesize_value = arg_uint64_value(cmd, stripesize_ARG, 0);
	}

	lp->ac_no_sync = arg_count(cmd, nosync_ARG);
	lp->ac_alloc = arg_uint_value(cmd, alloc_ARG, 0);

	lp->ac_type = arg_str_value(cmd, type_ARG, NULL);
	lp->ac_force = arg_count(cmd, force_ARG);

	return 1;
}

static int _adjust_policy_params(struct cmd_context *cmd,
				 struct logical_volume *lv, struct lvresize_params *lp)
{
	percent_t percent;
	int policy_threshold, policy_amount;

	if (lv_is_thin_pool(lv)) {
		policy_threshold =
			find_config_tree_int(cmd, activation_thin_pool_autoextend_threshold_CFG,
					     lv_config_profile(lv)) * PERCENT_1;
		policy_amount =
			find_config_tree_int(cmd, activation_thin_pool_autoextend_percent_CFG,
					     lv_config_profile(lv));
		if (!policy_amount && policy_threshold < PERCENT_100)
                        return 0;
	} else {
		policy_threshold =
			find_config_tree_int(cmd, activation_snapshot_autoextend_threshold_CFG, NULL) * PERCENT_1;
		policy_amount =
			find_config_tree_int(cmd, activation_snapshot_autoextend_percent_CFG, NULL);
	}

	if (policy_threshold >= PERCENT_100)
		return 1; /* nothing to do */

	if (lv_is_thin_pool(lv)) {
		if (!lv_thin_pool_percent(lv, 1, &percent))
			return_0;
		if ((PERCENT_0 < percent && percent <= PERCENT_100) &&
		    (percent > policy_threshold)) {
			if (!pool_can_resize_metadata(lv)) {
				log_error_once("Online metadata resize for %s/%s is not supported.",
					       lp->vg_name, lp->lv_name);
				return 0;
			}
			lp->poolmetadatasize = (first_seg(lv)->metadata_lv->size *
						policy_amount + 99) / 100;
			lp->poolmetadatasign = SIGN_PLUS;
		}

		if (!lv_thin_pool_percent(lv, 0, &percent))
			return_0;
		if (!(PERCENT_0 < percent && percent <= PERCENT_100) ||
		    percent <= policy_threshold)
			return 1;
	} else {
		if (!lv_snapshot_percent(lv, &percent))
			return_0;
		if (!(PERCENT_0 < percent && percent <= PERCENT_100) || percent <= policy_threshold)
			return 1; /* nothing to do */
	}

	lp->extents = policy_amount;
	lp->sizeargs = (lp->extents) ? 1 : 0;

	return 1;
}

static uint32_t lvseg_get_stripes(struct lv_segment *seg, uint32_t *stripesize)
{
	uint32_t s;
	struct lv_segment *seg_mirr;

	/* If segment mirrored, check if images are striped */
	if (seg_is_mirrored(seg))
		for (s = 0; s < seg->area_count; s++) {
			if (seg_type(seg, s) != AREA_LV)
				continue;
			seg_mirr = first_seg(seg_lv(seg, s));

			if (seg_is_striped(seg_mirr)) {
				seg = seg_mirr;
				break;
			}
		}


	if (seg_is_striped(seg)) {
		*stripesize = seg->stripe_size;
		return seg->area_count;
	}

	*stripesize = 0;
	return 0;
}

static int _lvresize_poolmetadata(struct cmd_context *cmd, struct volume_group *vg,
				  struct lvresize_params *lp,
				  const struct logical_volume *pool_lv,
				  struct dm_list *pvh,
				  alloc_policy_t alloc)
{
	struct logical_volume *lv;
	struct lv_segment *mseg;
	uint32_t extents;
	uint32_t seg_mirrors;

	if (!pool_can_resize_metadata(pool_lv)) {
		log_error("Support for online metadata resize not detected.");
		return 0;
	}

	if (lp->poolmetadatasize % vg->extent_size) {
		lp->poolmetadatasize += vg->extent_size -
			(lp->poolmetadatasize % vg->extent_size);
		log_print_unless_silent("Rounding pool metadata size to boundary between physical extents: %s",
					display_size(cmd, lp->poolmetadatasize));
	}

	if (!(extents = extents_from_size(vg->cmd, lp->poolmetadatasize,
					  vg->extent_size)))
		return_0;

	lv = first_seg(pool_lv)->metadata_lv;
	if (lp->poolmetadatasign == SIGN_PLUS) {
		if (extents >= (MAX_EXTENT_COUNT - lv->le_count)) {
			log_error("Unable to extend %s by %u extents, exceeds limit (%u).",
				  lv->name, lv->le_count, MAX_EXTENT_COUNT);
			return 0;
		}
		extents += lv->le_count;
	}

	if (extents * vg->extent_size > DM_THIN_MAX_METADATA_SIZE) {
		log_print_unless_silent("Rounding size to maximum supported size 16GiB "
					"for metadata volume %s.", lv->name);
		extents = (DM_THIN_MAX_METADATA_SIZE + vg->extent_size - 1) /
			vg->extent_size;
	}

	if (extents == lv->le_count) {
		log_print_unless_silent("Metadata volume %s has already %s.",
					lv->name, display_size(cmd, lv->size));
		return 2;
	}

	if (!lp->sizeargs && !archive(vg))
		return_0;

	log_print_unless_silent("Extending logical volume %s to %s.",
				lv->name,
				display_size(cmd, (uint64_t) extents * vg->extent_size));
	mseg = last_seg(lv);
	seg_mirrors = lv_mirror_count(lv);
	if (!lv_extend(lv,
		       mseg->segtype,
		       mseg->area_count / seg_mirrors,
		       mseg->stripe_size,
		       seg_mirrors,
		       mseg->region_size,
		       extents - lv->le_count, NULL,
		       pvh, alloc))
		return_0;

	return 1;
}

static int _lvresize(struct cmd_context *cmd, struct volume_group *vg,
		     struct lvresize_params *lp, struct dm_list *pvh)
{
	struct logical_volume *lv;
	uint32_t stripesize_extents;
	uint32_t seg_stripes = 0, seg_stripesize = 0, seg_size;
	uint32_t seg_mirrors = 0;
	uint32_t extents_used;
	uint32_t size_rest;
	uint32_t pv_extent_count;
	alloc_policy_t alloc;
	struct logical_volume *lock_lv = NULL;
	struct lv_list *lvl;
	struct lv_segment *seg, *uninitialized_var(mirr_seg);
	uint32_t seg_extents;
	uint32_t sz, str;
	int status;

	/* does LV exist? */
	if (!(lvl = find_lv_in_vg(vg, lp->lv_name))) {
		log_error("Logical volume %s not found in volume group %s",
			  lp->lv_name, lp->vg_name);
		return ECMD_FAILED;
	}

	lv = lvl->lv;

	if (lv_is_external_origin(lv)) {
		/*
		 * Since external-origin can be activated read-only,
		 * there is no way to use extended areas.
		 */
		log_error("Cannot resize external origin \"%s\".", lv->name);
		return EINVALID_CMD_LINE;
	}

	if (lv->status & (RAID_IMAGE | RAID_META)) {
		log_error("Cannot resize a RAID %s directly",
			  (lv->status & RAID_IMAGE) ? "image" :
			  "metadata area");
		return ECMD_FAILED;
	}

	if (lv_is_raid_with_tracking(lv)) {
		log_error("Cannot resize %s while it is tracking a split image",
			  lv->name);
		return ECMD_FAILED;
	}

	if (lp->ac_stripes) {
		if (vg->fid->fmt->features & FMT_SEGMENTS)
			lp->stripes = lp->ac_stripes_value;
		else
			log_warn("Varied striping not supported. Ignoring.");
	}

	if (lp->ac_mirrors) {
		if (vg->fid->fmt->features & FMT_SEGMENTS)
			lp->mirrors = lp->ac_mirrors_value;
		else
			log_warn("Mirrors not supported. Ignoring.");
	}

	if (lp->ac_stripesize &&
	    !_validate_stripesize(cmd, vg, lp))
		return EINVALID_CMD_LINE;

	if (lp->ac_policy) {
		if (!lv_is_cow(lv) &&
		    !lv_is_thin_pool(lv)) {
			log_error("Policy-based resize is supported only for snapshot and thin pool volumes.");
			return ECMD_FAILED;
		}
		if (!_adjust_policy_params(cmd, lv, lp))
			return_ECMD_FAILED;
	}

	if (!lv_is_visible(lv) &&
	    !lv_is_thin_pool_metadata(lv)) {
		log_error("Can't resize internal logical volume %s", lv->name);
		return ECMD_FAILED;
	}

	if (lv->status & LOCKED) {
		log_error("Can't resize locked LV %s", lv->name);
		return ECMD_FAILED;
	}

	if (lv->status & CONVERTING) {
		log_error("Can't resize %s while lvconvert in progress", lv->name);
		return ECMD_FAILED;
	}

	alloc = (alloc_policy_t)(lp->ac_alloc)?lp->ac_alloc: lv->alloc;

	/*
	 * First adjust to an exact multiple of extent size.
	 * When extending by a relative amount we round that amount up.
	 * When reducing by a relative amount we remove at most that amount.
	 * When changing to an absolute size, we round that size up.
	 */
	if (lp->size) {
		if (lp->size % vg->extent_size) {
			if (lp->sign == SIGN_MINUS)
				lp->size -= lp->size % vg->extent_size;
			else
				lp->size += vg->extent_size -
				    (lp->size % vg->extent_size);

			log_print_unless_silent("Rounding size to boundary between physical extents: %s",
						display_size(cmd, lp->size));
		}

		lp->extents = lp->size / vg->extent_size;
	}

	if (lp->sizeargs) { /* TODO: reindent or move to function */

	switch(lp->percent) {
		case PERCENT_VG:
			lp->extents = percent_of_extents(lp->extents, vg->extent_count,
							 (lp->sign != SIGN_MINUS));
			break;
		case PERCENT_FREE:
			lp->extents = percent_of_extents(lp->extents, vg->free_count,
							 (lp->sign != SIGN_MINUS));
			break;
		case PERCENT_LV:
			lp->extents = percent_of_extents(lp->extents, lv->le_count,
							 (lp->sign != SIGN_MINUS));
			break;
		case PERCENT_PVS:
			if (lp->argc) {
				pv_extent_count = pv_list_extents_free(pvh);
				lp->extents = percent_of_extents(lp->extents, pv_extent_count,
								 (lp->sign != SIGN_MINUS));
			} else
				lp->extents = percent_of_extents(lp->extents, vg->extent_count,
								 (lp->sign != SIGN_MINUS));
			break;
		case PERCENT_ORIGIN:
			if (!lv_is_cow(lv)) {
				log_error("Specified LV does not have an origin LV.");
				return EINVALID_CMD_LINE;
			}
			lp->extents = percent_of_extents(lp->extents, origin_from_cow(lv)->le_count,
							 (lp->sign != SIGN_MINUS));
			break;
		case PERCENT_NONE:
			break;
	}

	if (lp->sign == SIGN_PLUS) {
		if (lp->extents >= (MAX_EXTENT_COUNT - lv->le_count)) {
			log_error("Unable to extend %s by %u extents, exceeds limit (%u).",
				  lp->lv_name, lv->le_count, MAX_EXTENT_COUNT);
			return EINVALID_CMD_LINE;
		}
		lp->extents += lv->le_count;
		if (lv_is_cow(lv)) {
			extents_used = cow_max_extents(origin_from_cow(lv), find_cow(lv)->chunk_size);
			if (extents_used < lp->extents) {
				log_print_unless_silent("Reached maximum COW size %s.",
							display_size(vg->cmd, (uint64_t) vg->extent_size * extents_used));
				lp->extents = extents_used;
				if (lp->extents == lv->le_count)
					return ECMD_PROCESSED;
			}
		}
	} else if (lp->sign == SIGN_MINUS) {
		if (lp->extents >= lv->le_count) {
			log_error("Unable to reduce %s below 1 extent",
				  lp->lv_name);
			return EINVALID_CMD_LINE;
		}

		lp->extents = lv->le_count - lp->extents;
	}

	if (!lp->extents) {
		log_error("New size of 0 not permitted");
		return EINVALID_CMD_LINE;
	}

	if (lp->extents == lv->le_count) {
		/* A bit of hack - but still may resize metadata */
		if (lp->poolmetadatasize) {
			lp->sizeargs = 0;
			goto metadata_resize;
		}
		if (lp->ac_policy)
			return ECMD_PROCESSED; /* Nothing to do. */
		if (!lp->resizefs) {
			log_error("New size (%d extents) matches existing size "
				  "(%d extents)", lp->extents, lv->le_count);
			return EINVALID_CMD_LINE;
		}
		lp->resize = LV_EXTEND; /* lets pretend zero size extension */
	}

	seg_size = lp->extents - lv->le_count;

	/* Use segment type of last segment */
	lp->segtype = last_seg(lv)->segtype;

	/* FIXME Support LVs with mixed segment types */
	if (lp->segtype != get_segtype_from_string(cmd, (lp->ac_type)?lp->ac_type:lp->segtype->name)) {
		log_error("VolumeType does not match (%s)", lp->segtype->name);
		return EINVALID_CMD_LINE;
	}

	/* If extending, find mirrors of last segment */
	if ((lp->extents > lv->le_count)) {
		/*
		 * Has the user specified that they would like the additional
		 * extents of a mirror not to have an initial sync?
		 */
		if (seg_is_mirrored(first_seg(lv)) && lp->ac_no_sync)
			lv->status |= LV_NOTSYNCED;

		dm_list_iterate_back_items(mirr_seg, &lv->segments) {
			if (seg_is_mirrored(mirr_seg))
				seg_mirrors = lv_mirror_count(mirr_seg->lv);
			else
				seg_mirrors = 0;
			break;
		}

		if (!lp->ac_mirrors && seg_mirrors) {
			log_print_unless_silent("Extending %" PRIu32 " mirror images.",
						seg_mirrors);
			lp->mirrors = seg_mirrors;
		}
		if ((lp->ac_mirrors || seg_mirrors) &&
		    (lp->mirrors != seg_mirrors)) {
			log_error("Cannot vary number of mirrors in LV yet.");
			return EINVALID_CMD_LINE;
		}

		if (seg_mirrors && !strcmp(mirr_seg->segtype->name, "raid10")) {
			lp->stripes = mirr_seg->area_count / seg_mirrors;
			lp->stripe_size = mirr_seg->stripe_size;
		}
	}

	/* If extending, find stripes, stripesize & size of last segment */
	if ((lp->extents > lv->le_count) &&
	    !(lp->stripes == 1 || (lp->stripes > 1 && lp->stripe_size)) &&
	    strcmp(mirr_seg->segtype->name, "raid10")) {
		/* FIXME Don't assume mirror seg will always be AREA_LV */
		/* FIXME We will need to support resize for metadata LV as well,
		 *       and data LV could be any type (i.e. mirror)) */
		dm_list_iterate_items(seg, seg_mirrors ? &seg_lv(mirr_seg, 0)->segments :
				      lv_is_thin_pool(lv) ? &seg_lv(first_seg(lv), 0)->segments : &lv->segments) {
			/* Allow through "striped" and RAID 4/5/6/10 */
			if (!seg_is_striped(seg) &&
			    (!seg_is_raid(seg) || seg_is_mirrored(seg)) &&
			    strcmp(seg->segtype->name, "raid10"))
				continue;

			sz = seg->stripe_size;
			str = seg->area_count - lp->segtype->parity_devs;

			if ((seg_stripesize && seg_stripesize != sz &&
			     sz && !lp->stripe_size) ||
			    (seg_stripes && seg_stripes != str && !lp->stripes)) {
				log_error("Please specify number of "
					  "stripes (-i) and stripesize (-I)");
				return EINVALID_CMD_LINE;
			}

			seg_stripesize = sz;
			seg_stripes = str;
		}

		if (!lp->stripes)
			lp->stripes = seg_stripes;
		else if (seg_is_raid(first_seg(lv)) &&
			 (lp->stripes != seg_stripes)) {
			log_error("Unable to extend \"%s\" segment type with different number of stripes.", first_seg(lv)->segtype->ops->name(first_seg(lv)));
			return ECMD_FAILED;
		}

		if (!lp->stripe_size && lp->stripes > 1) {
			if (seg_stripesize) {
				log_print_unless_silent("Using stripesize of last segment %s",
							display_size(cmd, (uint64_t) seg_stripesize));
				lp->stripe_size = seg_stripesize;
			} else {
				lp->stripe_size =
					find_config_tree_int(cmd, metadata_stripesize_CFG, NULL) * 2;
				log_print_unless_silent("Using default stripesize %s",
							display_size(cmd, (uint64_t) lp->stripe_size));
			}
		}
	}

	/* If reducing, find stripes, stripesize & size of last segment */
	if (lp->extents < lv->le_count) {
		extents_used = 0;

		if (lp->stripes || lp->stripe_size || lp->mirrors)
			log_error("Ignoring stripes, stripesize and mirrors "
				  "arguments when reducing");

		dm_list_iterate_items(seg, &lv->segments) {
			seg_extents = seg->len;

			/* Check for underlying stripe sizes */
			seg_stripes = lvseg_get_stripes(seg, &seg_stripesize);

			if (seg_is_mirrored(seg))
				seg_mirrors = lv_mirror_count(seg->lv);
			else
				seg_mirrors = 0;

			if (lp->extents <= extents_used + seg_extents)
				break;

			extents_used += seg_extents;
		}

		seg_size = lp->extents - extents_used;
		lp->stripe_size = seg_stripesize;
		lp->stripes = seg_stripes;
		lp->mirrors = seg_mirrors;
	}

	if (lp->stripes > 1 && !lp->stripe_size) {
		log_error("Stripesize for striped segment should not be 0!");
		return EINVALID_CMD_LINE;
	}

	if (lp->stripes > 1) {
		if (lp->stripe_size < STRIPE_SIZE_MIN) {
			log_error("Invalid stripe size %s",
				  display_size(cmd, (uint64_t) lp->stripe_size));
			return EINVALID_CMD_LINE;
		}

		if (!(stripesize_extents = lp->stripe_size / vg->extent_size))
			stripesize_extents = 1;

		size_rest = seg_size % (lp->stripes * stripesize_extents);
		/* Round toward the original size. */
		if (size_rest &&
		    ((lp->extents < lv->le_count) ||
		     !lp->percent ||
		     (vg->free_count >= (lp->extents - lv->le_count - size_rest +
					 (lp->stripes * stripesize_extents))))) {
			log_print_unless_silent("Rounding size (%d extents) up to stripe "
						"boundary size for segment (%d extents)",
						lp->extents, lp->extents - size_rest +
						(lp->stripes * stripesize_extents));
			lp->extents = lp->extents - size_rest +
				      (lp->stripes * stripesize_extents);
		} else if (size_rest) {
			log_print_unless_silent("Rounding size (%d extents) down to stripe "
						"boundary size for segment (%d extents)",
						lp->extents, lp->extents - size_rest);
			lp->extents = lp->extents - size_rest;
		}
	}

	if (lp->extents < lv->le_count) {
		if (lp->resize == LV_EXTEND) {
			log_error("New size given (%d extents) not larger "
				  "than existing size (%d extents)",
				  lp->extents, lv->le_count);
			return EINVALID_CMD_LINE;
		}
		lp->resize = LV_REDUCE;
	} else if (lp->extents > lv->le_count) {
		if (lp->resize == LV_REDUCE) {
			log_error("New size given (%d extents) not less than "
				  "existing size (%d extents)", lp->extents,
				  lv->le_count);
			return EINVALID_CMD_LINE;
		}
		lp->resize = LV_EXTEND;
	} else if (lp->extents == lv->le_count) {
		if (lp->ac_policy)
			return ECMD_PROCESSED; /* Nothing to do. */
		if (!lp->resizefs) {
			log_error("New size (%d extents) matches existing size "
				  "(%d extents)", lp->extents, lv->le_count);
			return EINVALID_CMD_LINE;
		}
		lp->resize = LV_EXTEND;
	}

	if (lv_is_origin(lv)) {
		if (lp->resize == LV_REDUCE) {
			log_error("Snapshot origin volumes cannot be reduced "
				  "in size yet.");
			return ECMD_FAILED;
		}

		if (lv_is_active(lv)) {
			log_error("Snapshot origin volumes can be resized "
				  "only while inactive: try lvchange -an");
			return ECMD_FAILED;
		}
	}

	if (lv_is_thin_pool(lv)) {
		if (lp->resize == LV_REDUCE) {
			log_error("Thin pool volumes cannot be reduced in size yet.");
			return ECMD_FAILED;
		}

		if (lp->resizefs) {
			log_warn("Thin pool volumes do not have filesystem.");
			lp->resizefs = 0;
		}
	} else if (lp->poolmetadatasize) {
		log_error("--poolmetadatasize can be used only with thin pools.");
		return ECMD_FAILED;
	}

	if ((lp->resize == LV_REDUCE) && lp->argc)
		log_warn("Ignoring PVs on command line when reducing");

	/* Request confirmation before operations that are often mistakes. */
	if ((lp->resizefs || (lp->resize == LV_REDUCE)) &&
	    !_request_confirmation(cmd, vg, lv, lp))
		return_ECMD_FAILED;

	if (lp->resizefs) {
		if (!lp->nofsck &&
		    !_fsadm_cmd(cmd, vg, lp, FSADM_CMD_CHECK, &status)) {
			if (status != FSADM_CHECK_FAILS_FOR_MOUNTED) {
				log_error("Filesystem check failed.");
				return ECMD_FAILED;
			}
			/* some filesystems supports online resize */
		}

		if ((lp->resize == LV_REDUCE) &&
		    !_fsadm_cmd(cmd, vg, lp, FSADM_CMD_RESIZE, NULL)) {
			log_error("Filesystem resize failed.");
			return ECMD_FAILED;
		}
	}

	if (!archive(vg))
		return_ECMD_FAILED;

	log_print_unless_silent("%sing logical volume %s to %s",
				(lp->resize == LV_REDUCE) ? "Reduc" : "Extend",
				lv->name,
				display_size(cmd, (uint64_t) lp->extents * vg->extent_size));

	if (lp->resize == LV_REDUCE) {
		if (!lv_reduce(lv, lv->le_count - lp->extents))
			return ECMD_FAILED;
	} else if ((lp->extents > lv->le_count) && /* Ensure we extend */
		   !lv_extend(lv, lp->segtype,
			      lp->stripes, lp->stripe_size,
			      lp->mirrors, first_seg(lv)->region_size,
			      lp->extents - lv->le_count, NULL,
			      pvh, alloc))
		return_ECMD_FAILED;

	/* If thin metadata, must suspend thin pool */
	if (lv_is_thin_pool_metadata(lv)) {
		if (!(lock_lv = find_pool_lv(lv)))
			return_0;
	/* If snapshot, must suspend all associated devices */
	} else if (lv_is_cow(lv))
		lock_lv = origin_from_cow(lv);
	else
		lock_lv = lv;

	} /* lp->sizeargs */

	if (lp->poolmetadatasize) {
metadata_resize:
		if (!(status = _lvresize_poolmetadata(cmd, vg, lp, lv, pvh, alloc)))
			return_ECMD_FAILED;
		else if ((status == 2) && !lp->sizeargs)
			return ECMD_PROCESSED;
		lock_lv = lv;
	}

	if (!lock_lv)
		return ECMD_PROCESSED; /* Nothing to do */

	/* store vg on disk(s) */
	if (!vg_write(vg))
		return_ECMD_FAILED;

	if (!suspend_lv(cmd, lock_lv)) {
		log_error("Failed to suspend %s", lock_lv->name);
		vg_revert(vg);
		backup(vg);
		return ECMD_FAILED;
	}

	if (!vg_commit(vg)) {
		stack;
		if (!resume_lv(cmd, lock_lv))
			stack;
		backup(vg);
		return ECMD_FAILED;
	}

	if (!resume_lv(cmd, lock_lv)) {
		log_error("Problem reactivating %s", lock_lv->name);
		backup(vg);
		return ECMD_FAILED;
	}

	if (lv_is_cow_covering_origin(lv))
		if (!monitor_dev_for_events(cmd, lv, 0, 0))
			stack;

	backup(vg);

	/*
	 * Update lvm pool metadata (drop messages) if the pool has been
	 * resumed and do a pool active/deactivate in other case.
	 *
	 * Note: Active thin pool can be waiting for resize.
	 *
	 * FIXME: Activate only when thin volume is active
	 */
	if (lv_is_thin_pool(lv) &&
	    !update_pool_lv(lv, !lv_is_active(lv)))
		return_ECMD_FAILED;

	log_print_unless_silent("Logical volume %s successfully resized", lp->lv_name);

	if (lp->resizefs && (lp->resize == LV_EXTEND) &&
	    !_fsadm_cmd(cmd, vg, lp, FSADM_CMD_RESIZE, NULL))
		return_ECMD_FAILED;

	return ECMD_PROCESSED;
}

int lvresize(struct cmd_context *cmd, int argc, char **argv)
{
	struct lvresize_params lp = { 0 };
	struct volume_group *vg;
	int r;
	struct dm_list *pvh = NULL;

	if (!_lvresize_params(cmd, argc, argv, &lp))
		return EINVALID_CMD_LINE;

	log_verbose("Finding volume group %s", lp.vg_name);
	vg = vg_read_for_update(cmd, lp.vg_name, NULL, 0);
	if (vg_read_error(vg)) {
		release_vg(vg);
		return_ECMD_FAILED;
	}

	/* How does this list get cleaned up? */
	if (!(pvh = lp.argc ? create_pv_list(cmd->mem, vg, lp.argc,
						     lp.argv, 1) : &vg->pvs)) {
		return_ECMD_FAILED;
	}

	if (!(r = _lvresize(cmd, vg, &lp, pvh)))
		stack;

	unlock_and_release_vg(cmd, vg, lp.vg_name);

	return r;
}
