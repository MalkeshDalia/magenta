// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <magenta/device/devmgr.h>
#include <magenta/listnode.h>

#include <ddk/device.h>

#include <mxio/debug.h>
#include <mxio/vfs.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "vfs.h"
#include "dnode.h"

// Non-intrusive node in linked list of vnodes acting as mount points
typedef struct mount_node {
    list_node_t node;
    vnode_t* vn;
} mount_node_t;

static list_node_t remote_list = LIST_INITIAL_VALUE(remote_list);

mx_status_t vfs_install_remote(vnode_t* vn, mx_handle_t h) {
    if (vn == NULL) {
        return ERR_ACCESS_DENIED;
    }

    mtx_lock(&vfs_lock);
    // We cannot mount if anything else is already installed remotely
    if (vn->remote > 0) {
        mtx_unlock(&vfs_lock);
        return ERR_ALREADY_BOUND;
    }
    // Allocate a node to track the remote handle
    mount_node_t* mount_point;
    if ((mount_point = calloc(1, sizeof(mount_node_t))) == NULL) {
        mtx_unlock(&vfs_lock);
        return ERR_NO_MEMORY;
    }
    // Save this node in the list of mounted vnodes
    mount_point->vn = vn;
    list_add_tail(&remote_list, &mount_point->node);
    vn->remote = h;
    vn->flags |= V_FLAG_REMOTE;
    mtx_unlock(&vfs_lock);

    return NO_ERROR;
}

static mx_status_t txn_unmount(mx_handle_t srv) {
    mx_status_t r;
    mx_handle_t rchannel0, rchannel1;
    if ((r = mx_channel_create(MX_FLAG_REPLY_CHANNEL, &rchannel0, &rchannel1)) < 0) {
        return r;
    }
    mxrio_msg_t msg;
    memset(&msg, 0, MXRIO_HDR_SZ);
    msg.op = MXRIO_IOCTL;
    msg.arg2.op = IOCTL_DEVMGR_UNMOUNT_FS;
    if ((r = mxrio_txn_handoff(srv, rchannel0, &msg)) < 0) {
        return r;
    }
    if ((r = mx_handle_wait_one(rchannel1, MX_SIGNAL_PEER_CLOSED,
                                MX_TIME_INFINITE, NULL)) < 0) {
        return r;
    }
    return NO_ERROR;
}

mx_status_t vfs_uninstall_all(void) {
    mount_node_t* mount_point;
    mount_node_t* tmp;
    mtx_lock(&vfs_lock);
    list_for_every_entry_safe (&remote_list, mount_point, tmp, mount_node_t, node) {
        mx_status_t status;
        if ((status = txn_unmount(mount_point->vn->remote)) < 0) {
            printf("Unexpected error unmounting filesystem: %d\n", status);
        }
        mx_handle_close(mount_point->vn->remote);
        mount_point->vn->remote = 0;
        list_delete(&mount_point->node);
    }
    mtx_unlock(&vfs_lock);
    return NO_ERROR;
}