#!/usr/bin/env python3

# Copyright (C) 2015-2016 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

import dbus
# noinspection PyUnresolvedReferences
from dbus.mainloop.glib import DBusGMainLoop
import unittest
import sys
import time
import pyudev
import os
from testlib import *

use_session = os.getenv('LVMDBUSD_USE_SESSION', False)

if use_session:
	bus = dbus.SessionBus(mainloop=DBusGMainLoop())
else:
	bus = dbus.SystemBus(mainloop=DBusGMainLoop())


def get_objects():
	rc = {MANAGER_INT: [], PV_INT: [], VG_INT: [], LV_INT: [],
			THINPOOL_INT: [], JOB_INT: [], SNAPSHOT_INT: [], LV_COMMON_INT: [],
			CACHE_POOL_INT: [], CACHE_LV_INT: []}

	manager = dbus.Interface(bus.get_object(
		BUSNAME, "/com/redhat/lvmdbus1"),
		"org.freedesktop.DBus.ObjectManager")

	objects = manager.GetManagedObjects()

	for object_path, val in list(objects.items()):
		for interface, props in list(val.items()):
			o = ClientProxy(bus, object_path, interface, props)
			rc[interface].append(o)

	return rc, bus


def set_execution(lvmshell):
	lvm_manager = dbus.Interface(bus.get_object(
		BUSNAME, "/com/redhat/lvmdbus1/Manager"),
		"com.redhat.lvmdbus1.Manager")
	lvm_manager.UseLvmShell(lvmshell)


# noinspection PyUnresolvedReferences
class TestDbusService(unittest.TestCase):
	def setUp(self):
		# Because of the sensitive nature of running LVM tests we will only
		# run if we have PVs and nothing else, so that we can be confident that
		# we are not mucking with someones data on their system
		self.objs, self.bus = get_objects()
		if len(self.objs[PV_INT]) == 0:
			print('No PVs present exiting!')
			sys.exit(1)
		if len(self.objs[MANAGER_INT]) != 1:
			print('Expecting a manager object!')
			sys.exit(1)

		if len(self.objs[VG_INT]) != 0:
			print('Expecting no VGs to exist!')
			sys.exit(1)

		self.pvs = []
		for p in self.objs[PV_INT]:
			self.pvs.append(p.Pv.Name)

	def tearDown(self):
		# If we get here it means we passed setUp, so lets remove anything
		# and everything that remains, besides the PVs themselves
		self.objs, self.bus = get_objects()
		for v in self.objs[VG_INT]:
			# print "DEBUG: Removing VG= ", v.Uuid, v.Name
			v.Vg.Remove(-1, {})

		# Check to make sure the PVs we had to start exist, else re-create
		# them
		if len(self.pvs) != len(self.objs[PV_INT]):
			for p in self.pvs:
				found = False
				for pc in self.objs[PV_INT]:
					if pc.Pv.Name == p:
						found = True
						break

				if not found:
					# print('Re-creating PV=', p)
					self._pv_create(p)

	def _pv_create(self, device):
		pv_path = self.objs[MANAGER_INT][0].Manager.PvCreate(device, -1, {})[0]
		self.assertTrue(pv_path is not None and len(pv_path) > 0)
		return pv_path

	def _manager(self):
		return self.objs[MANAGER_INT][0]

	def _refresh(self):
		return self._manager().Manager.Refresh()

	def test_refresh(self):
		rc = self._refresh()
		self.assertEqual(rc, 0)

	def test_version(self):
		rc = self.objs[MANAGER_INT][0].Manager.Version
		self.assertTrue(rc is not None and len(rc) > 0)
		self.assertEqual(self._refresh(), 0)

	def _vg_create(self, pv_paths=None):

		if not pv_paths:
			pv_paths = [self.objs[PV_INT][0].object_path]

		vg_name = rs(8, '_vg')

		vg_path = self.objs[MANAGER_INT][0].Manager.VgCreate(
			vg_name,
			pv_paths,
			-1,
			{})[0]
		self.assertTrue(vg_path is not None and len(vg_path) > 0)
		return ClientProxy(self.bus, vg_path)

	def test_vg_create(self):
		self._vg_create()
		self.assertEqual(self._refresh(), 0)

	def test_vg_delete(self):
		vg = self._vg_create().Vg
		vg.Remove(-1, {})
		self.assertEqual(self._refresh(), 0)

	def _pv_remove(self, pv):
		rc = pv.Pv.Remove(-1, {})
		return rc

	def test_pv_remove_add(self):
		target = self.objs[PV_INT][0]

		# Remove the PV
		rc = self._pv_remove(target)
		self.assertTrue(rc == '/')
		self.assertEqual(self._refresh(), 0)

		# Add it back
		rc = self._pv_create(target.Pv.Name)[0]
		self.assertTrue(rc == '/')
		self.assertEqual(self._refresh(), 0)

	def _create_raid5_thin_pool(self, vg=None):

		if not vg:
			pv_paths = []
			for pp in self.objs[PV_INT]:
				pv_paths.append(pp.object_path)

			vg = self._vg_create(pv_paths).Vg

		lv_meta_path = vg.LvCreateRaid(
			"meta_r5", "raid5", mib(4), 0, 0, -1, {})[0]

		lv_data_path = vg.LvCreateRaid(
			"data_r5", "raid5", mib(16), 0, 0, -1, {})[0]

		thin_pool_path = vg.CreateThinPool(
			lv_meta_path, lv_data_path, -1, {})[0]

		# Get thin pool client proxy
		thin_pool = ClientProxy(self.bus, thin_pool_path)

		return vg, thin_pool

	def test_meta_lv_data_lv_props(self):
		# Ensure that metadata lv and data lv for thin pools and cache pools
		# point to a valid LV
		(vg, thin_pool) = self._create_raid5_thin_pool()

		# Check properties on thin pool
		self.assertTrue(thin_pool.ThinPool.DataLv != '/')
		self.assertTrue(thin_pool.ThinPool.MetaDataLv != '/')

		(vg, cache_pool) = self._create_cache_pool(vg)

		self.assertTrue(cache_pool.CachePool.DataLv != '/')
		self.assertTrue(cache_pool.CachePool.MetaDataLv != '/')

		# Cache the thin pool
		cached_thin_pool_path = cache_pool.\
			CachePool.CacheLv(thin_pool.object_path, -1, {})[0]

		# Get object proxy for cached thin pool
		cached_thin_pool_object = ClientProxy(self.bus, cached_thin_pool_path)

		# Check properties on cache pool
		self.assertTrue(cached_thin_pool_object.ThinPool.DataLv != '/')
		self.assertTrue(cached_thin_pool_object.ThinPool.MetaDataLv != '/')

	def _lookup(self, lvm_id):
		return self.objs[MANAGER_INT][0].Manager.LookUpByLvmId(lvm_id)

	def test_lookup_by_lvm_id(self):
		# For the moment lets just lookup what we know about which is PVs
		# When we start testing VGs and LVs we will test lookups for those
		# during those unit tests
		for p in self.objs[PV_INT]:
			rc = self._lookup(p.Pv.Name)
			self.assertTrue(rc is not None and rc != '/')

		# Search for something which doesn't exist
		rc = self._lookup('/dev/null')
		self.assertTrue(rc == '/')

	def test_vg_extend(self):
		# Create a VG
		self.assertTrue(len(self.objs[PV_INT]) >= 2)

		if len(self.objs[PV_INT]) >= 2:
			pv_initial = self.objs[PV_INT][0]
			pv_next = self.objs[PV_INT][1]

			vg = self._vg_create([pv_initial.object_path]).Vg
			path = vg.Extend([pv_next.object_path], -1, {})
			self.assertTrue(path == '/')
			self.assertEqual(self._refresh(), 0)

	# noinspection PyUnresolvedReferences
	def test_vg_reduce(self):
		self.assertTrue(len(self.objs[PV_INT]) >= 2)

		if len(self.objs[PV_INT]) >= 2:
			vg = self._vg_create(
				[self.objs[PV_INT][0].object_path,
				self.objs[PV_INT][1].object_path]).Vg

			path = vg.Reduce(False, [vg.Pvs[0]], -1, {})
			self.assertTrue(path == '/')
			self.assertEqual(self._refresh(), 0)

	# noinspection PyUnresolvedReferences
	def test_vg_rename(self):
		vg = self._vg_create().Vg

		mgr = self.objs[MANAGER_INT][0].Manager

		# Do a vg lookup
		path = mgr.LookUpByLvmId(vg.Name)

		vg_name_start = vg.Name

		prev_path = path
		self.assertTrue(path != '/', "%s" % (path))

		# Create some LVs in the VG
		for i in range(0, 5):
			lv_t = self._create_lv(size=mib(4), vg=vg)
			full_name = "%s/%s" % (vg_name_start, lv_t.LvCommon.Name)
			lv_path = mgr.LookUpByLvmId(full_name)
			self.assertTrue(lv_path == lv_t.object_path)

		new_name = 'renamed_' + vg.Name

		path = vg.Rename(new_name, -1, {})
		self.assertTrue(path == '/')
		self.assertEqual(self._refresh(), 0)

		# Do a vg lookup
		path = mgr.LookUpByLvmId(new_name)
		self.assertTrue(path != '/', "%s" % (path))
		self.assertTrue(prev_path == path, "%s != %s" % (prev_path, path))

		# Go through each LV and make sure it has the correct path back to the
		# VG
		vg.update()

		lv_paths = vg.Lvs
		self.assertTrue(len(lv_paths) == 5)

		for l in lv_paths:
			lv_proxy = ClientProxy(self.bus, l).LvCommon
			self.assertTrue(lv_proxy.Vg == vg.object_path, "%s != %s" %
							(lv_proxy.Vg, vg.object_path))
			full_name = "%s/%s" % (new_name, lv_proxy.Name)
			lv_path = mgr.LookUpByLvmId(full_name)
			self.assertTrue(lv_path == lv_proxy.object_path, "%s != %s" %
							(lv_path, lv_proxy.object_path))

	def _verify_hidden_lookups(self, lv_common_object, vgname):
		mgr = self.objs[MANAGER_INT][0].Manager

		hidden_lv_paths = lv_common_object.HiddenLvs

		for h in hidden_lv_paths:
			h_lv = ClientProxy(self.bus, h).LvCommon

			if len(h_lv.HiddenLvs) > 0:
				self._verify_hidden_lookups(h_lv, vgname)

			# print("Hidden check %s %s" % (h, h_lv.Name))
			full_name = "%s/%s" % (vgname, h_lv.Name)
			lookup_path = mgr.LookUpByLvmId(full_name)
			self.assertTrue(lookup_path != '/')
			self.assertTrue(lookup_path == h_lv.object_path)

	def test_vg_rename_with_thin_pool(self):

		(vg, thin_pool) = self._create_raid5_thin_pool()

		vg_name_start = vg.Name
		mgr = self.objs[MANAGER_INT][0].Manager

		# noinspection PyTypeChecker
		self._verify_hidden_lookups(thin_pool.LvCommon, vg_name_start)

		for i in range(0, 5):
			lv_name = rs(8, '_lv')

			thin_lv_path = thin_pool.ThinPool.LvCreate(
				lv_name, mib(16), -1, {})[0]

			self.assertTrue(thin_lv_path != '/')

			full_name = "%s/%s" % (vg_name_start, lv_name)

			lookup_lv_path = mgr.LookUpByLvmId(full_name)
			self.assertTrue(thin_lv_path == lookup_lv_path,
							"%s != %s" % (thin_lv_path, lookup_lv_path))

		# Rename the VG
		new_name = 'renamed_' + vg.Name

		path = vg.Rename(new_name, -1, {})
		self.assertTrue(path == '/')
		self.assertEqual(self._refresh(), 0)

		# Go through each LV and make sure it has the correct path back to the
		# VG
		vg.update()
		thin_pool.update()

		lv_paths = vg.Lvs

		for l in lv_paths:
			lv_proxy = ClientProxy(self.bus, l).LvCommon
			self.assertTrue(lv_proxy.Vg == vg.object_path, "%s != %s" %
							(lv_proxy.Vg, vg.object_path))
			full_name = "%s/%s" % (new_name, lv_proxy.Name)
			# print('Full Name %s' % (full_name))
			lv_path = mgr.LookUpByLvmId(full_name)
			self.assertTrue(lv_path == lv_proxy.object_path, "%s != %s" %
							(lv_path, lv_proxy.object_path))

		# noinspection PyTypeChecker
		self._verify_hidden_lookups(thin_pool.LvCommon, new_name)

	def _test_lv_create(self, method, params, vg):
		lv = None
		path = method(*params)[0]

		self.assertTrue(vg)

		if path:
			lv = ClientProxy(self.bus, path)
			# TODO verify object properties

		# We are quick enough now that we can get VolumeType changes from
		# 'I' to 'i' between the time it takes to create a RAID and it returns
		# and when we refresh state here.  Not sure how we can handle this as
		# we cannot just sit and poll all the time for changes...
		# self.assertEqual(self._refresh(), 0)
		return lv

	def test_lv_create(self):
		vg = self._vg_create().Vg
		self._test_lv_create(
			vg.LvCreate,
			(rs(8, '_lv'), mib(4),
			dbus.Array([], '(ott)'), -1, {}), vg)

	def test_lv_create_linear(self):

		vg = self._vg_create().Vg
		self._test_lv_create(
			vg.LvCreateLinear,
			(rs(8, '_lv'), mib(4), False, -1, {}), vg)

	def test_lv_create_striped(self):
		pv_paths = []
		for pp in self.objs[PV_INT]:
			pv_paths.append(pp.object_path)

		vg = self._vg_create(pv_paths).Vg
		self._test_lv_create(
			vg.LvCreateStriped,
			(rs(8, '_lv'), mib(4), 2, 8, False,
			-1, {}), vg)

	def test_lv_create_mirror(self):
		pv_paths = []
		for pp in self.objs[PV_INT]:
			pv_paths.append(pp.object_path)

		vg = self._vg_create(pv_paths).Vg
		self._test_lv_create(vg.LvCreateMirror,
			(rs(8, '_lv'), mib(4), 2, -1, {}), vg)

	def test_lv_create_raid(self):
		pv_paths = []
		for pp in self.objs[PV_INT]:
			pv_paths.append(pp.object_path)

		vg = self._vg_create(pv_paths).Vg
		self._test_lv_create(vg.LvCreateRaid,
			(rs(8, '_lv'), 'raid4',
			mib(16), 2, 8, -1, {}), vg)

	def _create_lv(self, thinpool=False, size=None, vg=None):

		if not vg:
			pv_paths = []
			for pp in self.objs[PV_INT]:
				pv_paths.append(pp.object_path)

			vg = self._vg_create(pv_paths).Vg

		if size is None:
			size = mib(4)

		return self._test_lv_create(
			vg.LvCreateLinear,
			(rs(8, '_lv'), size, thinpool, -1, {}), vg)

	def test_lv_create_rounding(self):
		self._create_lv(size=(mib(2) + 13))

	def test_lv_create_thin_pool(self):
		self._create_lv(True)

	def test_lv_rename(self):
		# Rename a regular LV
		lv = self._create_lv()

		path = self.objs[MANAGER_INT][0].Manager.LookUpByLvmId(lv.LvCommon.Name)
		prev_path = path

		new_name = 'renamed_' + lv.LvCommon.Name
		lv.Lv.Rename(new_name, -1, {})

		path = self.objs[MANAGER_INT][0].Manager.LookUpByLvmId(new_name)

		self.assertEqual(self._refresh(), 0)
		self.assertTrue(prev_path == path, "%s != %s" % (prev_path, path))

	def test_lv_thinpool_rename(self):
		# Rename a thin pool
		tp = self._create_lv(True)
		self.assertTrue(THINPOOL_LV_PATH in tp.object_path,
						"%s" % (tp.object_path))

		new_name = 'renamed_' + tp.LvCommon.Name
		tp.Lv.Rename(new_name, -1, {})
		tp.update()
		self.assertEqual(self._refresh(), 0)
		self.assertEqual(new_name, tp.LvCommon.Name)

	# noinspection PyUnresolvedReferences
	def test_lv_on_thin_pool_rename(self):
		# Rename a LV on a thin Pool

		# This returns a LV with the LV interface, need to get a proxy for
		# thinpool interface too
		tp = self._create_lv(True)

		thin_path = tp.ThinPool.LvCreate(
			rs(10, '_thin_lv'), mib(8), -1, {})[0]

		lv = ClientProxy(self.bus, thin_path)
		rc = lv.Lv.Rename('rename_test' + lv.LvCommon.Name, -1, {})
		self.assertTrue(rc == '/')
		self.assertEqual(self._refresh(), 0)

	def test_lv_remove(self):
		lv = self._create_lv().Lv
		rc = lv.Remove(-1, {})
		self.assertTrue(rc == '/')
		self.assertEqual(self._refresh(), 0)

	def test_lv_snapshot(self):
		lv_p = self._create_lv()
		ss_name = 'ss_' + lv_p.LvCommon.Name

		# Test waiting to complete
		ss, job = lv_p.Lv.Snapshot(ss_name, 0, -1, {})
		self.assertTrue(ss != '/')
		self.assertTrue(job == '/')

		snapshot = ClientProxy(self.bus, ss)
		self.assertTrue(snapshot.LvCommon.Name == ss_name)

		self.assertEqual(self._refresh(), 0)

		# Test getting a job returned immediately
		rc, job = lv_p.Lv.Snapshot('ss2_' + lv_p.LvCommon.Name, 0, 0, {})
		self.assertTrue(rc == '/')
		self.assertTrue(job != '/')
		self._wait_for_job(job)

		self.assertEqual(self._refresh(), 0)

	# noinspection PyUnresolvedReferences
	def _wait_for_job(self, j_path):
		import time
		rc = None
		j = ClientProxy(self.bus, j_path).Job

		while True:
			j.update()
			if j.Complete:
				(ec, error_msg) = j.GetError
				self.assertTrue(ec == 0, "%d :%s" % (ec, error_msg))

				if ec == 0:
					self.assertTrue(j.Percent == 100, "P= %f" % j.Percent)

				rc = j.Result
				j.Remove()

				break

			if j.Wait(1):
				j.update()
				self.assertTrue(j.Complete)

		return rc

	def test_lv_create_pv_specific(self):
		vg = self._vg_create().Vg

		pv = vg.Pvs

		pv_proxy = ClientProxy(self.bus, pv[0])

		self._test_lv_create(vg.LvCreate, (rs(8, '_lv'), mib(4),
			dbus.Array([[pv_proxy.object_path, 0, (pv_proxy.Pv.PeCount - 1)]],
			'(ott)'), -1, {}), vg)

	def test_lv_resize(self):

		pv_paths = []
		for pp in self.objs[PV_INT]:
			pv_paths.append(pp.object_path)

		vg = self._vg_create(pv_paths).Vg
		lv = self._create_lv(vg=vg, size=mib(16))

		for size in \
			[
			lv.LvCommon.SizeBytes + 4194304,
			lv.LvCommon.SizeBytes - 4194304,
			lv.LvCommon.SizeBytes + 2048,
			lv.LvCommon.SizeBytes - 2048]:

			pv_in_use = [i[0] for i in lv.LvCommon.Devices]
			# Select a PV in the VG that isn't in use
			pv_empty = [p for p in vg.Pvs if p not in pv_in_use]

			prev = lv.LvCommon.SizeBytes

			if len(pv_empty):
				p = ClientProxy(self.bus, pv_empty[0])
				rc = lv.Lv.Resize(
					size,
					dbus.Array([[p.object_path, 0, p.Pv.PeCount - 1]], '(oii)'),
					-1, {})
			else:
				rc = lv.Lv.Resize(size, dbus.Array([], '(oii)'), -1, {})

			self.assertEqual(rc, '/')
			self.assertEqual(self._refresh(), 0)

			lv.update()

			if prev < size:
				self.assertTrue(lv.LvCommon.SizeBytes > prev)
			else:
				# We are testing re-sizing to same size too...
				self.assertTrue(lv.LvCommon.SizeBytes <= prev)

	def test_lv_resize_same(self):
		pv_paths = []
		for pp in self.objs[PV_INT]:
			pv_paths.append(pp.object_path)

		vg = self._vg_create(pv_paths).Vg
		lv = self._create_lv(vg=vg)

		with self.assertRaises(dbus.exceptions.DBusException):
			lv.Lv.Resize(lv.LvCommon.SizeBytes, dbus.Array([], '(oii)'), -1, {})

	def test_lv_move(self):
		lv = self._create_lv()

		pv_path_move = str(lv.LvCommon.Devices[0][0])

		# Test moving a specific LV
		job = lv.Lv.Move(pv_path_move, (0, 0), dbus.Array([], '(oii)'), 0, {})
		self._wait_for_job(job)
		self.assertEqual(self._refresh(), 0)

		lv.update()
		new_pv = str(lv.LvCommon.Devices[0][0])
		self.assertTrue(pv_path_move != new_pv, "%s == %s" %
						(pv_path_move, new_pv))

	def test_lv_activate_deactivate(self):
		lv_p = self._create_lv()
		lv_p.update()

		lv_p.Lv.Deactivate(0, -1, {})
		lv_p.update()
		self.assertFalse(lv_p.LvCommon.Active)
		self.assertEqual(self._refresh(), 0)

		lv_p.Lv.Activate(0, -1, {})

		lv_p.update()
		self.assertTrue(lv_p.LvCommon.Active)
		self.assertEqual(self._refresh(), 0)

		# Try control flags
		for i in range(0, 5):
			lv_p.Lv.Activate(1 << i, -1, {})
			self.assertTrue(lv_p.LvCommon.Active)
			self.assertEqual(self._refresh(), 0)

	def test_move(self):
		lv = self._create_lv()

		# Test moving without being LV specific
		vg = ClientProxy(self.bus, lv.LvCommon.Vg).Vg
		pv_to_move = str(lv.LvCommon.Devices[0][0])
		job = vg.Move(pv_to_move, (0, 0), dbus.Array([], '(oii)'), 0, {})
		self._wait_for_job(job)
		self.assertEqual(self._refresh(), 0)

		# Test Vg.Move
		# TODO Test this more!
		vg.update()
		lv.update()

		location = lv.LvCommon.Devices[0][0]

		dst = None
		for p in vg.Pvs:
			if p != location:
				dst = p

		# Fetch the destination
		pv = ClientProxy(self.bus, dst).Pv

		# Test range, move it to the middle of the new destination and blocking
		# blocking for it to complete
		job = vg.Move(
			location, (0, 0), [(dst, pv.PeCount / 2, 0), ], -1, {})
		self.assertEqual(job, '/')
		self.assertEqual(self._refresh(), 0)

	def test_job_handling(self):
		pv_paths = []
		for pp in self.objs[PV_INT]:
			pv_paths.append(pp.object_path)

		vg_name = rs(8, '_vg')

		# Test getting a job right away
		vg_path, vg_job = self.objs[MANAGER_INT][0].Manager.VgCreate(
			vg_name, pv_paths,
			0, {})

		self.assertTrue(vg_path == '/')
		self.assertTrue(vg_job and len(vg_job) > 0)

		self._wait_for_job(vg_job)

	def _test_expired_timer(self, num_lvs):
		rc = False
		pv_paths = []
		for pp in self.objs[PV_INT]:
			pv_paths.append(pp.object_path)

		# In small configurations lvm is pretty snappy, so lets create a VG
		# add a number of LVs and then remove the VG and all the contained
		# LVs which appears to consistently run a little slow.

		vg_proxy = self._vg_create(pv_paths)

		for i in range(0, num_lvs):

			vg_proxy.update()
			if vg_proxy.Vg.FreeCount > 0:
				obj_path, job = vg_proxy.Vg.LvCreateLinear(
					rs(8, "_lv"), mib(4), False, -1, {})
				self.assertTrue(job == '/')
			else:
				# We ran out of space, test will probably fail
				break

		# Make sure that we are honoring the timeout
		start = time.time()

		remove_job = vg_proxy.Vg.Remove(1, {})

		end = time.time()

		tt_remove = float(end) - float(start)

		self.assertTrue(tt_remove < 2.0, "remove time %s" % (str(tt_remove)))

		# Depending on how long it took we could finish either way
		if remove_job != '/':
			# We got a job
			result = self._wait_for_job(remove_job)
			self.assertTrue(result == '/')
			rc = True
		else:
			# It completed before timer popped
			pass

		return rc

	def test_job_handling_timer(self):

		yes = False

		# This may not pass
		for i in [48, 64, 128]:
			yes = self._test_expired_timer(i)
			if yes:
				break
			print('Attempt (%d) failed, trying again...' % (i))

		self.assertTrue(yes)

	def test_pv_tags(self):
		pvs = []

		pv_paths = []
		for pp in self.objs[PV_INT]:
			pv_paths.append(pp.object_path)

		vg = self._vg_create(pv_paths).Vg

		# Get the PVs
		for p in vg.Pvs:
			pvs.append(ClientProxy(self.bus, p).Pv)

		for tags_value in [['hello'], ['foo', 'bar']]:
			rc = vg.PvTagsAdd(vg.Pvs, tags_value, -1, {})
			self.assertTrue(rc == '/')

			for p in pvs:
				p.update()
				self.assertTrue(sorted(tags_value) == p.Tags)

			vg.PvTagsDel(vg.Pvs, tags_value, -1, {})
			for p in pvs:
				p.update()
				self.assertTrue([] == p.Tags)

	def test_vg_tags(self):
		vg = self._vg_create().Vg

		t = ['Testing', 'tags']

		vg.TagsAdd(t, -1, {})
		vg.update()
		self.assertTrue(t == vg.Tags)
		vg.TagsDel(t, -1, {})
		vg.update()
		self.assertTrue([] == vg.Tags)

	def test_lv_tags(self):
		vg = self._vg_create().Vg
		lv = self._test_lv_create(
			vg.LvCreateLinear,
			(rs(8, '_lv'), mib(4), False, -1, {}),
			vg)

		t = ['Testing', 'tags']

		lv.Lv.TagsAdd(t, -1, {})
		lv.update()
		self.assertTrue(t == lv.LvCommon.Tags)
		lv.Lv.TagsDel(t, -1, {})
		lv.update()
		self.assertTrue([] == lv.LvCommon.Tags)

	def test_vg_allocation_policy_set(self):
		vg = self._vg_create().Vg

		for p in ['anywhere', 'contiguous', 'cling', 'normal']:
			rc = vg.AllocationPolicySet(p, -1, {})
			self.assertEqual(rc, '/')
			vg.update()

			prop = getattr(vg, 'Alloc' + p.title())
			self.assertTrue(prop)

	def test_vg_max_pv(self):
		vg = self._vg_create().Vg

		# BZ: https://bugzilla.redhat.com/show_bug.cgi?id=1280496
		# TODO: Add a test back for larger values here when bug is resolved
		for p in [0, 1, 10, 100, 100, 1024, 2 ** 32 - 1]:
			rc = vg.MaxPvSet(p, -1, {})
			self.assertEqual(rc, '/')
			vg.update()
			self.assertTrue(vg.MaxPv == p, "Expected %s != Actual %s" %
							(str(p), str(vg.MaxPv)))

	def test_vg_max_lv(self):
		vg = self._vg_create().Vg

		# BZ: https://bugzilla.redhat.com/show_bug.cgi?id=1280496
		# TODO: Add a test back for larger values here when bug is resolved
		for p in [0, 1, 10, 100, 100, 1024, 2 ** 32 - 1]:
			rc = vg.MaxLvSet(p, -1, {})
			self.assertEqual(rc, '/')
			vg.update()
			self.assertTrue(vg.MaxLv == p, "Expected %s != Actual %s" %
							(str(p), str(vg.MaxLv)))

	def test_vg_uuid_gen(self):
		# TODO renable test case when
		# https://bugzilla.redhat.com/show_bug.cgi?id=1264169 gets fixed
		# This was tested with lvmetad disabled and we passed
		print("\nSkipping Vg.UuidGenerate until BZ: 1264169 resolved\n")

		if False:
			vg = self._vg_create().Vg
			prev_uuid = vg.Uuid
			rc = vg.UuidGenerate(-1, {})
			self.assertEqual(rc, '/')
			vg.update()
			self.assertTrue(vg.Uuid != prev_uuid, "Expected %s != Actual %s" %
							(vg.Uuid, prev_uuid))

	def test_vg_activate_deactivate(self):
		vg = self._vg_create().Vg
		self._test_lv_create(
			vg.LvCreateLinear,
			(rs(8, '_lv'), mib(4), False, -1, {}),
			vg)

		vg.update()

		vg.Deactivate(0, -1, {})
		self.assertEqual(self._refresh(), 0)

		vg.Activate(0, -1, {})
		self.assertEqual(self._refresh(), 0)

		# Try control flags
		for i in range(0, 5):
			vg.Activate(1 << i, -1, {})

	def test_pv_resize(self):

		self.assertTrue(len(self.objs[PV_INT]) > 0)

		if len(self.objs[PV_INT]) > 0:
			pv = ClientProxy(self.bus, self.objs[PV_INT][0].object_path).Pv

			original_size = pv.SizeBytes

			new_size = original_size / 2

			pv.ReSize(new_size, -1, {})
			self.assertEqual(self._refresh(), 0)
			pv.update()

			self.assertTrue(pv.SizeBytes != original_size)
			pv.ReSize(0, -1, {})
			self.assertEqual(self._refresh(), 0)
			pv.update()
			self.assertTrue(pv.SizeBytes == original_size)

	def test_pv_allocation(self):

		pv_paths = []
		for pp in self.objs[PV_INT]:
			pv_paths.append(pp.object_path)

		vg = self._vg_create(pv_paths).Vg

		pv = ClientProxy(self.bus, vg.Pvs[0]).Pv

		pv.AllocationEnabled(False, -1, {})
		pv.update()
		self.assertFalse(pv.Allocatable)

		pv.AllocationEnabled(True, -1, {})
		pv.update()
		self.assertTrue(pv.Allocatable)

		self.assertEqual(self._refresh(), 0)

	def _get_devices(self):
		context = pyudev.Context()
		return context.list_devices(subsystem='block', MAJOR='8')

	def test_pv_scan(self):
		devices = self._get_devices()

		mgr = self._manager().Manager

		self.assertEqual(
			mgr.PvScan(
				False, True, dbus.Array([], 's'),
				dbus.Array([], '(ii)'), -1, {}), '/')
		self.assertEqual(self._refresh(), 0)
		self.assertEqual(
			mgr.PvScan(
				False, False,
				dbus.Array([], 's'),
				dbus.Array([], '(ii)'), -1, {}), '/')
		self.assertEqual(self._refresh(), 0)

		block_path = []
		for d in devices:
			block_path.append(d['DEVNAME'])

		self.assertEqual(mgr.PvScan(False, True,
									block_path,
									dbus.Array([], '(ii)'), -1, {}), '/')

		self.assertEqual(self._refresh(), 0)

		mm = []
		for d in devices:
			mm.append((int(d['MAJOR']), int(d['MINOR'])))

		self.assertEqual(mgr.PvScan(False, True,
									block_path,
									mm, -1, {}), '/')

		self.assertEqual(self._refresh(), 0)

		self.assertEqual(
			mgr.PvScan(
				False, True,
				dbus.Array([], 's'),
				mm, -1, {}), '/')

		self.assertEqual(self._refresh(), 0)

	@staticmethod
	def _write_some_data(device_path, size):
		blocks = int(size / 512)
		block = bytearray(512)
		for i in range(0, 512):
			block[i] = i % 255

		with open(device_path, mode='wb') as lv:
			for i in range(0, blocks):
				lv.write(block)

	def test_snapshot_merge(self):
		# Create a non-thin LV and merge it
		ss_size = mib(8)

		lv_p = self._create_lv(size=mib(16))
		ss_name = lv_p.LvCommon.Name + '_snap'
		snapshot_path = lv_p.Lv.Snapshot(ss_name, ss_size, -1, {})[0]
		ss = ClientProxy(self.bus, snapshot_path)

		# Write some data to snapshot so merge takes some time
		TestDbusService._write_some_data(ss.LvCommon.Path, ss_size / 2)

		job_path = ss.Snapshot.Merge(0, {})

		self.assertTrue(job_path != '/')
		self._wait_for_job(job_path)

	def test_snapshot_merge_thin(self):
		# Create a thin LV, snapshot it and merge it
		tp = self._create_lv(True)

		thin_path = tp.ThinPool.LvCreate(
			rs(10, '_thin_lv'), mib(10), -1, {})[0]

		lv_p = ClientProxy(self.bus, thin_path)

		ss_name = lv_p.LvCommon.Name + '_snap'
		snapshot_path = lv_p.Lv.Snapshot(ss_name, 0, -1, {})[0]
		ss = ClientProxy(self.bus, snapshot_path)
		job_path = ss.Snapshot.Merge(0, {})
		self.assertTrue(job_path != '/')
		self._wait_for_job(job_path)

	def _create_cache_pool(self, vg=None):

		if not vg:
			vg = self._vg_create().Vg

		md = self._create_lv(size=(mib(8)), vg=vg)
		data = self._create_lv(size=(mib(8)), vg=vg)

		cache_pool_path = vg.CreateCachePool(
			md.object_path, data.object_path, -1, {})[0]

		cp = ClientProxy(self.bus, cache_pool_path)

		return vg, cp

	def test_cache_pool_create(self):

		vg, cache_pool = self._create_cache_pool()

		self.assertTrue('/com/redhat/lvmdbus1/CachePool' in
						cache_pool.object_path)

	def test_cache_lv_create(self):

		for destroy_cache in [True, False]:
			vg, cache_pool = self._create_cache_pool()

			lv_to_cache = self._create_lv(size=mib(8), vg=vg)

			c_lv_path = cache_pool.CachePool.CacheLv(
				lv_to_cache.object_path, -1, {})[0]

			cached_lv = ClientProxy(self.bus, c_lv_path)

			uncached_lv_path = \
				cached_lv.CachedLv.DetachCachePool(destroy_cache, -1, {})[0]

			self.assertTrue('/com/redhat/lvmdbus1/Lv' in
							uncached_lv_path)

			vg.Remove(-1, {})

	def test_vg_change(self):
		vg_proxy = self._vg_create()
		result = vg_proxy.Vg.Change(-1, {'-a': 'ay'})
		self.assertTrue(result == '/')
		result = vg_proxy.Vg.Change(-1, {'-a': 'n'})
		self.assertTrue(result == '/')

	def _invalid_vg_lv_name_characters(self):
		bad_vg_lv_set = set(string.printable) - \
			set(string.ascii_letters + string.digits + '.-_+')
		return ''.join(bad_vg_lv_set)

	def test_invalid_names(self):
		mgr = self.objs[MANAGER_INT][0].Manager

		# Pv device path
		with self.assertRaises(dbus.exceptions.DBusException):
			mgr.PvCreate("/dev/space in name", -1, {})

		# VG Name testing...
		# Go through all bad characters
		pv_paths = [self.objs[PV_INT][0].object_path]
		bad_chars = self._invalid_vg_lv_name_characters()
		for c in bad_chars:
			with self.assertRaises(dbus.exceptions.DBusException):
				mgr.VgCreate("name%s" % (c), pv_paths, -1, {})

		# Bad names
		for bad in [".", ".."]:
			with self.assertRaises(dbus.exceptions.DBusException):
				mgr.VgCreate(bad, pv_paths, -1, {})

		# Exceed name length
		for i in [128, 1024, 4096]:
			with self.assertRaises(dbus.exceptions.DBusException):
				mgr.VgCreate('T' * i, pv_paths, -1, {})

		# Create a VG and try to create LVs with different bad names
		vg_path = mgr.VgCreate("test", pv_paths, -1, {})[0]
		vg_proxy = ClientProxy(self.bus, vg_path)

		for c in bad_chars:
			with self.assertRaises(dbus.exceptions.DBusException):
				vg_proxy.Vg.LvCreateLinear(rs(8, '_lv') + c,
					mib(4), False, -1, {})

		for r in ("_cdata", "_cmeta", "_corig", "_mimage", "_mlog",
			"_pmspare", "_rimage", "_rmeta", "_tdata", "_tmeta", "_vorigin"):
			with self.assertRaises(dbus.exceptions.DBusException):
				vg_proxy.Vg.LvCreateLinear(rs(8, '_lv') + r,
					mib(4), False, -1, {})

		for r in ("snapshot", "pvmove"):
			with self.assertRaises(dbus.exceptions.DBusException):
				vg_proxy.Vg.LvCreateLinear(r + rs(8, '_lv'),
					mib(4), False, -1, {})

	_ALLOWABLE_TAG_CH = string.ascii_letters + string.digits + "._-+/=!:&#"

	def _invalid_tag_characters(self):
		bad_tag_ch_set = set(string.printable) - set(self._ALLOWABLE_TAG_CH)
		return ''.join(bad_tag_ch_set)

	def test_invalid_tags(self):
		mgr = self.objs[MANAGER_INT][0].Manager
		pv_paths = [self.objs[PV_INT][0].object_path]

		vg_path = mgr.VgCreate("test", pv_paths, -1, {})[0]
		vg_proxy = ClientProxy(self.bus, vg_path)

		for c in self._invalid_tag_characters():
			with self.assertRaises(dbus.exceptions.DBusException):
				vg_proxy.Vg.TagsAdd([c], -1, {})

		for c in self._invalid_tag_characters():
			with self.assertRaises(dbus.exceptions.DBusException):
				vg_proxy.Vg.TagsAdd(["a%sb" % (c)], -1, {})

	def test_tag_names(self):
		mgr = self.objs[MANAGER_INT][0].Manager
		pv_paths = [self.objs[PV_INT][0].object_path]

		vg_path = mgr.VgCreate("test", pv_paths, -1, {})[0]
		vg_proxy = ClientProxy(self.bus, vg_path)

		for i in range(1, 64):
			tag = rs(i, "", self._ALLOWABLE_TAG_CH)
			r = vg_proxy.Vg.TagsAdd([tag], -1, {})
			self.assertTrue(r == '/')
			vg_proxy.update()

			self.assertTrue(tag in vg_proxy.Vg.Tags, "%s not in %s" %
				(tag, str(vg_proxy.Vg.Tags)))

			self.assertEqual(i, len(vg_proxy.Vg.Tags), "%d != %d" %
				(i, len(vg_proxy.Vg.Tags)))


if __name__ == '__main__':
	# Test forking & exec new each time
	test_shell = os.getenv('LVM_DBUS_TEST_SHELL', 0)

	set_execution(False)

	if test_shell == 0:
		unittest.main(exit=True)
	else:
		unittest.main(exit=False)
		# Test lvm shell
		print('\n *** Testing lvm shell *** \n')
		set_execution(True)
		unittest.main()
