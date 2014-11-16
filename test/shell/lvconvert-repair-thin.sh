#!/bin/sh

# Copyright (C) 2013-2014 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

# Test repairing of broken thin pool metadata

. lib/inittest

which mkfs.ext2 || skip

#
# Main
#
aux have_thin 1 0 0 || skip

aux prepare_vg 4

# Create LV
lvcreate -T -L20 -V10 -n $lv1 $vg/pool  "$dev1" "$dev2"
lvcreate -T -V10 -n $lv2 $vg/pool

mkfs.ext2 "$DM_DEV_DIR/$vg/$lv1"
mkfs.ext2 "$DM_DEV_DIR/$vg/$lv2"

lvcreate -L20 -n repair $vg
lvcreate -L2 -n fixed $vg

lvs -a -o+seg_pe_ranges $vg
#aux error_dev "$dev2" 2050:1

vgchange -an $vg

lvconvert --repair $vg/pool

lvs -a $vg

# Manual repair steps:
# Test swapping - swap out thin-pool's metadata with our repair volume
lvconvert -y -f --poolmetadata $vg/repair --thinpool $vg/pool

#
# To continue this test - we need real tools available
# When they are not present mark test as skipped, but still
# let proceed initial part which should work even without tools
#
aux have_tool_at_least "$LVM_TEST_THIN_CHECK_CMD" 0 3 1 || skip
aux have_tool_at_least "$LVM_TEST_THIN_DUMP_CMD" 0 3 1 || skip
aux have_tool_at_least "$LVM_TEST_THIN_REPAIR_CMD" 0 3 1 || skip

lvchange -aey $vg/repair $vg/fixed

# Make some 'repairable' damage??
dd if=/dev/zero of="$DM_DEV_DIR/$vg/repair" bs=1 seek=40960 count=1

not "$LVM_TEST_THIN_CHECK_CMD" "$DM_DEV_DIR/$vg/repair"

not "$LVM_TEST_THIN_DUMP_CMD" "$DM_DEV_DIR/$vg/repair" | tee dump

"$LVM_TEST_THIN_REPAIR_CMD" -i "$DM_DEV_DIR/$vg/repair" -o "$DM_DEV_DIR/$vg/fixed"

"$LVM_TEST_THIN_DUMP_CMD" --repair "$DM_DEV_DIR/$vg/repair" | tee repaired_xml

"$LVM_TEST_THIN_CHECK_CMD" "$DM_DEV_DIR/$vg/fixed"

# Swap repaired metadata back
lvconvert -y -f --poolmetadata $vg/fixed --thinpool $vg/pool

# Activate pool - this should now work
vgchange -ay $vg

vgchange -an $vg

# Put back 'broken' metadata
lvconvert -y -f --poolmetadata $vg/repair --thinpool $vg/pool

# Check --repair usage
lvconvert -v --repair $vg/pool

# Check repaired pool could be activated
lvchange -ay $vg/pool

lvchange -an $vg

# Restore damaged metadata
lvconvert -y -f --poolmetadata $vg/pool_meta1 --thinpool $vg/pool

# Check lvremove -ff works even with damaged pool
lvremove -ff $vg
