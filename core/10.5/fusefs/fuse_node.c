/*
 * Copyright (C) 2006 Google. All Rights Reserved.
 * Amit Singh <singh@>
 */

#include "fuse.h"
#include "fuse_internal.h"
#include "fuse_ipc.h"
#include "fuse_locking.h"
#include "fuse_node.h"
#include "fuse_sysctl.h"

void
FSNodeScrub(struct fuse_vnode_data *fvdat)
{
    lck_mtx_destroy(fvdat->createlock, fuse_lock_group);
    lck_rw_destroy(fvdat->nodelock, fuse_lock_group);
    lck_rw_destroy(fvdat->truncatelock, fuse_lock_group);
    fvdat->fMagic = kFSNodeBadMagic;
}       

errno_t
FSNodeGetOrCreateFileVNodeByID(mount_t       mp,
                               vfs_context_t context,
                               uint64_t      nodeid,
                               vnode_t       dvp,
                               enum vtype    vtyp,
                               uint64_t      insize,
                               vnode_t      *vnPtr,
                               int           flags,
                               int          *oflags,
                               uint32_t      rdev)
{
    int      err;
    int      junk;
    vnode_t  vn;
    HNodeRef hn;
    vnode_t  dirVN;
    dev_t    dummy_device;
    struct fuse_vnode_data *fvdat = NULL;
    struct fuse_data *mntdata;
    int markroot = FALSE;
    uint64_t size = 0;

    hn = NULL;
    vn = NULL;
    dirVN = NULL;

    if ((vtyp >= VBAD) || (vtyp < 0)) {
        return EINVAL;
    }

    if (insize == FUSE_ROOT_SIZE) {
        markroot = TRUE;
    } else {
        size = insize;
    }

    mntdata = vfs_fsprivate(mp);
    dummy_device = (dev_t)mntdata->fdev;

    err = HNodeLookupCreatingIfNecessary(dummy_device,
                                         (ino_t)nodeid, /* XXXXXXXXXX */
                                         0              /* fork index */,
                                         &hn,
                                         &vn);
    if ((err == 0) && (vn == NULL)) {

        struct vnode_fsparam params;
        fvdat = (struct fuse_vnode_data *)FSNodeGenericFromHNode(hn);
        if (!fvdat->fInitialised) {
            int k;
            fvdat->fMagic = kFSNodeMagic;
            fvdat->fInitialised = TRUE;
            fvdat->nid = nodeid;
            fvdat->vtype = vtyp;
            fvdat->parent = NULL;
            fvdat->filesize = size;
            fvdat->nlookup = 0;
            if (dvp) {
                fvdat->parent_nid = VTOFUD(dvp)->nid;
            } else {
                fvdat->parent_nid = 0;
            }
            for (k = 0; k < FUFH_MAXTYPE; k++) {
                fvdat->fufh[k].fufh_flags = 0;
            }
            fvdat->createlock = lck_mtx_alloc_init(fuse_lock_group, fuse_lock_attr);
            fvdat->nodelock = lck_rw_alloc_init(fuse_lock_group, fuse_lock_attr);
            fvdat->truncatelock = lck_rw_alloc_init(fuse_lock_group, fuse_lock_attr);
            fvdat->creator = current_thread();
            fvdat->flag = flags;
        }

        if (err == 0) {
            params.vnfs_mp         = mp;
            params.vnfs_vtype      = vtyp;
            params.vnfs_str        = NULL;

            params.vnfs_dvp        = dvp;
            if (markroot == TRUE) {
                params.vnfs_dvp = NULLVP; /* XXX: should be this coming in */
            }

            params.vnfs_fsnode     = hn;

#if M_MACFUSE_ENABLE_SPECFS
            if ((vtyp == VBLK) || (vtyp == VCHR)) {
                params.vnfs_vops   = fuse_spec_operations;
            } else {
                params.vnfs_vops   = fuse_vnode_operations;
            }
            params.vnfs_rdev       = (dev_t)rdev;
#else
            (void)rdev;
            params.vnfs_vops       = fuse_vnode_operations;
            params.vnfs_rdev       = 0;
#endif

            params.vnfs_marksystem = FALSE;
            params.vnfs_cnp        = NULL;
            params.vnfs_flags      = VNFS_NOCACHE | VNFS_CANTCACHE;
            params.vnfs_filesize   = size;
            params.vnfs_markroot   = markroot;

            err = vnode_create(VNCREATE_FLAVOR, sizeof(params), &params, &vn);
        }

        if (err == 0) {
            if (markroot == TRUE) {
                fvdat->parent = vn;
            } else {
                fvdat->parent = dvp;
            }
            if (oflags) {
                *oflags |= MAKEENTRY;
            }
            HNodeAttachVNodeSucceeded(hn, 0 /* forkIndex */, vn);
            FUSE_OSAddAtomic(1, (SInt32 *)&fuse_vnodes_current);
        } else {
            if (HNodeAttachVNodeFailed(hn, 0 /* forkIndex */)) {
                FSNodeScrub(fvdat);
                HNodeScrubDone(hn);
            }
        }
    }

    if (err == 0) {
        if (vnode_vtype(vn) != vtyp) {
            fuse_internal_vnode_disappear(vn, context, 1);
            vnode_put(vn);
            err = EAGAIN;
        }
    }

    if (err == 0) {
        *vnPtr = vn;
        vnode_settag(vn, VT_KERNFS);
    }

    if (dirVN != NULL) {
        junk = vnode_put(dirVN);
        /* assert(junk == 0); */
    }

    /* assert((err == 0) == (*vnPtr != NULL); */

    return err;
}

int
fuse_vget_i(mount_t               mp,
            uint64_t              nodeid,
            vfs_context_t         context,
            vnode_t               dvp,
            vnode_t              *vpp,
            struct componentname *cnp,
            enum vtype            vtyp,
            uint64_t              size,
   __unused enum vget_mode        mode,
   __unused uint64_t              parentid,
            uint32_t              rdev)
{
    int err = 0;

    debug_printf("dvp=%p\n", dvp);

    if (vtyp == VNON) {
        return EINVAL;
    }

#if M_MACFUSE_EXPERIMENTAL_JUNK
    if (nodeid == FUSE_ROOT_ID) {
        *vpp = fuse_get_mpdata(mp)->rvp; //XROOT
        err = vnode_get(*vpp);
        if (err) {
            *vpp = NULLVP;
            return err;
        }
        goto found;
    }
#endif

    err = FSNodeGetOrCreateFileVNodeByID(mp, context, nodeid, dvp,
                                         vtyp, size, vpp, 0, NULL, rdev);
    if (err) {
        return err;
    }

    if (!fuse_isnovncache_mp(mp) && (cnp->cn_flags & MAKEENTRY)) {
        fuse_vncache_enter(dvp, *vpp, cnp);
    }

/* found: */
    VTOFUD(*vpp)->nlookup++;

    return 0;
}