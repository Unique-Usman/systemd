/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright Tom Gundersen <teg@jklm.no>
***/

#include "sd-hwdb.h"

#include "alloc-util.h"
#include "hwdb-util.h"
#include "libudev-private.h"

/**
 * SECTION:libudev-hwdb
 * @short_description: retrieve properties from the hardware database
 *
 * Libudev hardware database interface.
 */

/**
 * udev_hwdb:
 *
 * Opaque object representing the hardware database.
 */
struct udev_hwdb {
        struct udev *udev;
        int refcount;

        sd_hwdb *hwdb;

        struct udev_list properties_list;
};

/**
 * udev_hwdb_new:
 * @udev: udev library context
 *
 * Create a hardware database context to query properties for devices.
 *
 * Returns: a hwdb context.
 **/
_public_ struct udev_hwdb *udev_hwdb_new(struct udev *udev) {
        _cleanup_(sd_hwdb_unrefp) sd_hwdb *hwdb_internal = NULL;
        struct udev_hwdb *hwdb;
        int r;

        assert_return_errno(udev, NULL, EINVAL);

        r = sd_hwdb_new(&hwdb_internal);
        if (r < 0) {
                errno = -r;
                return NULL;
        }

        hwdb = new0(struct udev_hwdb, 1);
        if (!hwdb) {
                errno = ENOMEM;
                return NULL;
        }

        hwdb->refcount = 1;
        hwdb->hwdb = TAKE_PTR(hwdb_internal);

        udev_list_init(udev, &hwdb->properties_list, true);

        return hwdb;
}

/**
 * udev_hwdb_ref:
 * @hwdb: context
 *
 * Take a reference of a hwdb context.
 *
 * Returns: the passed enumeration context
 **/
_public_ struct udev_hwdb *udev_hwdb_ref(struct udev_hwdb *hwdb) {
        if (!hwdb)
                return NULL;
        hwdb->refcount++;
        return hwdb;
}

/**
 * udev_hwdb_unref:
 * @hwdb: context
 *
 * Drop a reference of a hwdb context. If the refcount reaches zero,
 * all resources of the hwdb context will be released.
 *
 * Returns: #NULL
 **/
_public_ struct udev_hwdb *udev_hwdb_unref(struct udev_hwdb *hwdb) {
        if (!hwdb)
                return NULL;
        hwdb->refcount--;
        if (hwdb->refcount > 0)
                return NULL;
        sd_hwdb_unref(hwdb->hwdb);
        udev_list_cleanup(&hwdb->properties_list);
        return mfree(hwdb);
}

/**
 * udev_hwdb_get_properties_list_entry:
 * @hwdb: context
 * @modalias: modalias string
 * @flags: (unused)
 *
 * Lookup a matching device in the hardware database. The lookup key is a
 * modalias string, whose formats are defined for the Linux kernel modules.
 * Examples are: pci:v00008086d00001C2D*, usb:v04F2pB221*. The first entry
 * of a list of retrieved properties is returned.
 *
 * Returns: a udev_list_entry.
 */
_public_ struct udev_list_entry *udev_hwdb_get_properties_list_entry(struct udev_hwdb *hwdb, const char *modalias, unsigned int flags) {
        const char *key, *value;
        struct udev_list_entry *e;

        if (!hwdb || !modalias) {
                errno = EINVAL;
                return NULL;
        }

        udev_list_cleanup(&hwdb->properties_list);

        SD_HWDB_FOREACH_PROPERTY(hwdb->hwdb, modalias, key, value) {
                if (udev_list_entry_add(&hwdb->properties_list, key, value) == NULL) {
                        errno = ENOMEM;
                        return NULL;
                }
        }

        e = udev_list_get_entry(&hwdb->properties_list);
        if (!e)
                errno = ENODATA;

        return e;
}
