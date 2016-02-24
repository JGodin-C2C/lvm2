/*
 * Copyright (C) 2001-2004 Sistina Software, Inc. All rights reserved.  
 * Copyright (C) 2004-2007 Red Hat, Inc. All rights reserved.
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

/*
 * This file must be included first by every library source file.
 */
#ifndef _LVM_LIB_H
#define _LVM_LIB_H

/* HM FIXME: REMOVEME: devel output */
#if 0
#include "dump.h"
#endif

/* HM FIXME: REMOVEME: devel output */
#if 0
#define PFL() fprintf(stderr, "%s %u\n", __func__, __LINE__);
#define PFLA(format, arg...) fprintf(stderr, "%s %u " format "\n", __func__, __LINE__, arg);
#else
#define PFL() ;
#define PFLA(format, arg...) ;
#endif


#include "configure.h"

#define _REENTRANT
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#if defined(__GNUC__)
#define DM_EXPORTED_SYMBOL(func, ver) \
	__asm__(".symver " #func "_v" #ver ", " #func "@@DM_" #ver )
#define DM_EXPORTED_SYMBOL_BASE(func) \
	__asm__(".symver " #func "_base, " #func "@Base" )
#else
#define DM_EXPORTED_SYMBOL(func, ver)
#define DM_EXPORTED_SYMBOL_BASE(func)
#endif


#include "intl.h"
#include "libdevmapper.h"
#include "util.h"

#ifdef DM
#  include "dm-logging.h"
#else
#  include "lvm-logging.h"
#  include "lvm-globals.h"
#  include "lvm-wrappers.h"
#endif

#include <unistd.h>

#endif
