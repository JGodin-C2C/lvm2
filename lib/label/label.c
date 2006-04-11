/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "lib.h"
#include "label.h"
#include "crc.h"
#include "xlate.h"
#include "lvmcache.h"
#include "metadata.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* FIXME Allow for larger labels?  Restricted to single sector currently */

/*
 * Internal labeller struct.
 */
struct labeller_i {
	struct list list;

	struct labeller *l;
	char name[0];
};

static struct list _labellers;

static struct labeller_i *_alloc_li(const char *name, struct labeller *l)
{
	struct labeller_i *li;
	size_t len;

	len = sizeof(*li) + strlen(name) + 1;

	if (!(li = dm_malloc(len))) {
		log_error("Couldn't allocate memory for labeller list object.");
		return NULL;
	}

	li->l = l;
	strcpy(li->name, name);

	return li;
}

static void _free_li(struct labeller_i *li)
{
	dm_free(li);
}

int label_init(void)
{
	list_init(&_labellers);
	return 1;
}

void label_exit(void)
{
	struct list *c, *n;
	struct labeller_i *li;

	for (c = _labellers.n; c != &_labellers; c = n) {
		n = c->n;
		li = list_item(c, struct labeller_i);
		li->l->ops->destroy(li->l);
		_free_li(li);
	}

	list_init(&_labellers);
}

int label_register_handler(const char *name, struct labeller *handler)
{
	struct labeller_i *li;

	if (!(li = _alloc_li(name, handler))) {
		stack;
		return 0;
	}

	list_add(&_labellers, &li->list);
	return 1;
}

struct labeller *label_get_handler(const char *name)
{
	struct labeller_i *li;

	list_iterate_items(li, &_labellers)
		if (!strcmp(li->name, name))
			return li->l;

	return NULL;
}

static struct labeller *_find_labeller(struct device *dev, char *buf,
				       uint64_t *label_sector)
{
	struct labeller_i *li;
	struct labeller *r = NULL;
	struct label_header *lh;
	struct lvmcache_info *info;
	uint64_t sector;
	int found = 0;
	char readbuf[LABEL_SCAN_SIZE];

	if (!dev_read(dev, UINT64_C(0), LABEL_SCAN_SIZE, readbuf)) {
		log_debug("%s: Failed to read label area", dev_name(dev));
		goto out;
	}

	/* Scan first few sectors for a valid label */
	for (sector = 0; sector < LABEL_SCAN_SECTORS;
	     sector += LABEL_SIZE >> SECTOR_SHIFT) {
		lh = (struct label_header *) (readbuf +
					      (sector << SECTOR_SHIFT));

		if (!strncmp(lh->id, LABEL_ID, sizeof(lh->id))) {
			if (found) {
				log_error("Ignoring additional label on %s at "
					  "sector %" PRIu64, dev_name(dev),
					  sector);
			}
			if (xlate64(lh->sector_xl) != sector) {
				log_info("%s: Label for sector %" PRIu64
					 " found at sector %" PRIu64
					 " - ignoring", dev_name(dev),
					 xlate64(lh->sector_xl), sector);
				continue;
			}
			if (calc_crc(INITIAL_CRC, &lh->offset_xl, LABEL_SIZE -
				     ((void *) &lh->offset_xl - (void *) lh)) !=
			    xlate32(lh->crc_xl)) {
				log_info("Label checksum incorrect on %s - "
					 "ignoring", dev_name(dev));
				continue;
			}
			if (found)
				continue;
		}

		list_iterate_items(li, &_labellers) {
			if (li->l->ops->can_handle(li->l, (char *) lh, sector)) {
				log_very_verbose("%s: %s label detected",
						 dev_name(dev), li->name);
				if (found) {
					log_error("Ignoring additional label "
						  "on %s at sector %" PRIu64,
						  dev_name(dev), sector);
					continue;
				}
				r = li->l;
				memcpy(buf, lh, LABEL_SIZE);
				if (label_sector)
					*label_sector = sector;
				found = 1;
				break;
			}
		}
	}

      out:
	if (!found) {
		if ((info = info_from_pvid(dev->pvid)))
			lvmcache_update_vgname_and_id(info, ORPHAN, NULL);
		log_very_verbose("%s: No label detected", dev_name(dev));
	}

	return r;
}

/* FIXME Also wipe associated metadata area headers? */
int label_remove(struct device *dev)
{
	char buf[LABEL_SIZE];
	char readbuf[LABEL_SCAN_SIZE];
	int r = 1;
	uint64_t sector;
	int wipe;
	struct labeller_i *li;
	struct label_header *lh;

	memset(buf, 0, LABEL_SIZE);

	log_very_verbose("Scanning for labels to wipe from %s", dev_name(dev));

	if (!dev_open(dev)) {
		stack;
		return 0;
	}

	/*
	 * We flush the device just in case someone is stupid
	 * enough to be trying to import an open pv into lvm.
	 */
	dev_flush(dev);

	if (!dev_read(dev, UINT64_C(0), LABEL_SCAN_SIZE, readbuf)) {
		log_debug("%s: Failed to read label area", dev_name(dev));
		goto out;
	}

	/* Scan first few sectors for anything looking like a label */
	for (sector = 0; sector < LABEL_SCAN_SECTORS;
	     sector += LABEL_SIZE >> SECTOR_SHIFT) {
		lh = (struct label_header *) (readbuf +
					      (sector << SECTOR_SHIFT));

		wipe = 0;

		if (!strncmp(lh->id, LABEL_ID, sizeof(lh->id))) {
			if (xlate64(lh->sector_xl) == sector)
				wipe = 1;
		} else {
			list_iterate_items(li, &_labellers) {
				if (li->l->ops->can_handle(li->l, (char *) lh,
							   sector)) {
					wipe = 1;
					break;
				}
			}
		}

		if (wipe) {
			log_info("%s: Wiping label at sector %" PRIu64,
				 dev_name(dev), sector);
			if (!dev_write(dev, sector << SECTOR_SHIFT, LABEL_SIZE,
				       buf)) {
				log_error("Failed to remove label from %s at "
					  "sector %" PRIu64, dev_name(dev),
					  sector);
				r = 0;
			}
		}
	}

      out:
	if (!dev_close(dev))
		stack;

	return r;
}

/* FIXME Avoid repeated re-reading if cache lock held */
int label_read(struct device *dev, struct label **result)
{
	char buf[LABEL_SIZE];
	struct labeller *l;
	uint64_t sector;
	struct lvmcache_info *info;
	int r = 0;

	if (!dev_open(dev)) {
		stack;

		if ((info = info_from_pvid(dev->pvid)))
			lvmcache_update_vgname_and_id(info, ORPHAN, NULL);

		goto out;
	}

	if (!(l = _find_labeller(dev, buf, &sector)))
		goto_out;

	if ((r = (l->ops->read)(l, dev, buf, result)) && result && *result)
		(*result)->sector = sector;

      out:
	if (!dev_close(dev))
		stack;

	return r;
}

/* Caller may need to use label_get_handler to create label struct! */
int label_write(struct device *dev, struct label *label)
{
	char buf[LABEL_SIZE];
	struct label_header *lh = (struct label_header *) buf;
	int r = 1;

	if (!label->labeller->ops->write) {
		log_err("Label handler does not support label writes");
		return 0;
	}

	if ((LABEL_SIZE + (label->sector << SECTOR_SHIFT)) > LABEL_SCAN_SIZE) {
		log_error("Label sector %" PRIu64 " beyond range (%ld)",
			  label->sector, LABEL_SCAN_SECTORS);
		return 0;
	}

	memset(buf, 0, LABEL_SIZE);

	strncpy(lh->id, LABEL_ID, sizeof(lh->id));
	lh->sector_xl = xlate64(label->sector);
	lh->offset_xl = xlate32(sizeof(*lh));

	if (!(label->labeller->ops->write)(label, buf)) {
		stack;
		return 0;
	}

	lh->crc_xl = xlate32(calc_crc(INITIAL_CRC, &lh->offset_xl, LABEL_SIZE -
				      ((void *) &lh->offset_xl - (void *) lh)));

	if (!dev_open(dev)) {
		stack;
		return 0;
	}

	log_info("%s: Writing label to sector %" PRIu64, dev_name(dev),
		 label->sector);
	if (!dev_write(dev, label->sector << SECTOR_SHIFT, LABEL_SIZE, buf)) {
		log_debug("Failed to write label to %s", dev_name(dev));
		r = 0;
	}

	if (!dev_close(dev))
		stack;

	return r;
}

/* Unused */
int label_verify(struct device *dev)
{
	struct labeller *l;
	char buf[LABEL_SIZE];
	uint64_t sector;
	struct lvmcache_info *info;
	int r = 0;

	if (!dev_open(dev)) {
		stack;

		if ((info = info_from_pvid(dev->pvid)))
			lvmcache_update_vgname_and_id(info, ORPHAN, NULL);

		goto out;
	}

	if (!(l = _find_labeller(dev, buf, &sector)))
		goto_out;

	r = l->ops->verify ? l->ops->verify(l, buf, sector) : 1;

      out:
	if (!dev_close(dev))
		stack;

	return r;
}

void label_destroy(struct label *label)
{
	label->labeller->ops->destroy_label(label->labeller, label);
	dm_free(label);
}

struct label *label_create(struct labeller *labeller)
{
	struct label *label;

	if (!(label = dm_malloc(sizeof(*label)))) {
		log_error("label allocaction failed");
		return NULL;
	}
	memset(label, 0, sizeof(*label));

	label->labeller = labeller;

	labeller->ops->initialise_label(labeller, label);

	return label;
}
