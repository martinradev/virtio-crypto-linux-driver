/*
 * Virtio crypto device manager
 *
 * Based on adf_dev_mgr.c
 *
 * Authors: Gonglei <arei.gonglei@huawei.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 */

#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/module.h>

#include "virtio_crypto_common.h"

static LIST_HEAD(virtio_crypto_table);
static DEFINE_MUTEX(table_lock);
static uint32_t num_devices;

#define VIRTIO_CRYPTO_MAX_DEVICES 32


/**
 * virtcrypto_devmgr_add_dev() - Add vcrypto_dev to the acceleration
 * framework.
 * @vcrypto_dev:  Pointer to virtio crypto device.
 *
 * Function adds virtio crypto device to the global list.
 * To be used by virtio crypto device specific drivers.
 *
 * Return: 0 on success, error code othewise.
 */
int virtcrypto_devmgr_add_dev(struct virtio_crypto *vcrypto_dev)
{
	struct list_head *itr;

	if (num_devices == VIRTIO_CRYPTO_MAX_DEVICES) {
		pr_info("Only support up to %d devices\n",
			    VIRTIO_CRYPTO_MAX_DEVICES);
		return -EFAULT;
	}

	mutex_lock(&table_lock);
	list_for_each(itr, &virtio_crypto_table) {
		struct virtio_crypto *ptr =
				list_entry(itr, struct virtio_crypto, list);

		if (ptr == vcrypto_dev) {
			mutex_unlock(&table_lock);
			return -EEXIST;
		}
	}
	atomic_set(&vcrypto_dev->ref_count, 0);
	list_add_tail(&vcrypto_dev->list, &virtio_crypto_table);
	vcrypto_dev->dev_id = num_devices++;
	mutex_unlock(&table_lock);
	return 0;
}

struct list_head *virtcrypto_devmgr_get_head(void)
{
	return &virtio_crypto_table;
}

/**
 * virtcrypto_devmgr_rm_dev() - Remove accel_dev from the acceleration
 * framework.
 * @vcrypto_dev:  Pointer to virtio crypto device.
 *
 * Function removes virtio crypto device from the acceleration framework.
 * To be used by virtio crypto device specific drivers.
 *
 * Return: void
 */
void virtcrypto_devmgr_rm_dev(struct virtio_crypto *vcrypto_dev)
{
	mutex_lock(&table_lock);
	list_del(&vcrypto_dev->list);
	num_devices--;
	mutex_unlock(&table_lock);
}

struct virtio_crypto *virtcrypto_devmgr_get_first(void)
{
	struct virtio_crypto *dev = NULL;

	if (!list_empty(&virtio_crypto_table))
		dev = list_first_entry(&virtio_crypto_table, struct virtio_crypto,
				       list);
	return dev;
}

/**
 * virtcrypto_dev_in_use() - Check whether vcrypto_dev is currently in use
 * @vcrypto_dev: Pointer to acceleration device.
 *
 * To be used by virtio crypto device specific drivers.
 *
 * Return: 1 when device is in use, 0 otherwise.
 */
int virtcrypto_dev_in_use(struct virtio_crypto *vcrypto_dev)
{
	return atomic_read(&vcrypto_dev->ref_count) != 0;
}

/**
 * virtcrypto_dev_get() - Increment vcrypto_dev reference count
 * @accel_dev: Pointer to acceleration device.
 *
 * Increment the vcrypto_dev refcount and if this is the first time
 * incrementing it during this period the vcrypto_dev is in use,
 * increment the module refcount too.
 * To be used by virtio crypto device specific drivers.
 *
 * Return: 0 when successful, EFAULT when fail to bump module refcount
 */
int virtcrypto_dev_get(struct virtio_crypto *vcrypto_dev)
{
	if (atomic_add_return(1, &vcrypto_dev->ref_count) == 1)
		if (!try_module_get(vcrypto_dev->owner))
			return -EFAULT;
	return 0;
}

/**
 * virtcrypto_dev_put() - Decrement vcrypto_dev reference count
 * @accel_dev: Pointer to acceleration device.
 *
 * Decrement the vcrypto_dev refcount and if this is the last time
 * decrementing it during this period the vcrypto_dev is in use,
 * decrement the module refcount too.
 * To be used by virtio crypto device specific drivers.
 *
 * Return: void
 */
void virtcrypto_dev_put(struct virtio_crypto *vcrypto_dev)
{
	if (atomic_sub_return(1, &vcrypto_dev->ref_count) == 0)
		module_put(vcrypto_dev->owner);
}

/**
 * virtcrypto_dev_started() - Check whether device has started
 * @vcrypto_dev: Pointer to acceleration device.
 *
 * To be used by virtio crypto device specific drivers.
 *
 * Return: 1 when the device has started, 0 otherwise
 */
int virtcrypto_dev_started(struct virtio_crypto *vcrypto_dev)
{
	return (vcrypto_dev->status & VIRTIO_CRYPTO_S_STARTED);
}

struct virtio_crypto *virtcrypto_get_dev_node(int node)
{
	struct virtio_crypto *vcrypto_dev = NULL, *tmp_dev;
	unsigned long best = ~0;

	list_for_each_entry(tmp_dev, virtcrypto_devmgr_get_head(), list) {
		unsigned long ctr;

		if ((node == dev_to_node(&tmp_dev->vdev->dev) ||
		     dev_to_node(&tmp_dev->vdev->dev) < 0) &&
		    virtcrypto_dev_started(tmp_dev)) {
			ctr = atomic_read(&tmp_dev->ref_count);
			if (best > ctr) {
				vcrypto_dev = tmp_dev;
				best = ctr;
			}
		}
	}

	if (!vcrypto_dev) {
		pr_info("virtio-crypto: Could not find a device on node %d\n", node);
		/* Get any started device */
		list_for_each_entry(tmp_dev, virtcrypto_devmgr_get_head(), list) {
			if (virtcrypto_dev_started(tmp_dev)) {
				vcrypto_dev = tmp_dev;
				break;
			}
		}
	}

	if (!vcrypto_dev) {
		return NULL;
	}

	virtcrypto_dev_get(vcrypto_dev);
	return vcrypto_dev;
}

/**
 * virtcrypto_dev_start() - Start virtio crypto device
 * @vcrypto:    Pointer to virtio crypto device.
 *
 * Function notifies all the registered services that the acceleration device
 * is ready to be used.
 * To be used by virtio crypto device specific drivers.
 *
 * Return: 0 on success, error code otherwise.
 */
int virtcrypto_dev_start(struct virtio_crypto *vcrypto)
{
	if (virtio_crypto_algs_register()) {
		pr_err("virtio-crypto: Failed to register crypto algs\n");
		return -EFAULT;
	}

	vcrypto->status |= VIRTIO_CRYPTO_S_STARTED;
	return 0;
}

/**
 * virtcrypto_dev_stop() - Stop virtio crypto device
 * @vcrypto:    Pointer to virtio crypto device.
 *
 * Function notifies all the registered services that the acceleration device
 * is ready to be used.
 * To be used by virtio crypto device specific drivers.
 *
 * Return: 0 on success, error code otherwise.
 */
int virtcrypto_dev_stop(struct virtio_crypto *vcrypto)
{
	virtio_crypto_algs_unregister();

	vcrypto->status &= ~VIRTIO_CRYPTO_S_STARTED;

	return 0;
}