/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hal/hal_flash.h"
#include "os/os_mempool.h"
#include "os/os_malloc.h"
#include "nffs/nffs.h"
#include "nffs_priv.h"

/**
 * The size of the largest data block encountered during detection.  This is
 * used to ensure that the maximum block data size is not set lower than the
 * size of an existing block.
 */
static uint16_t nffs_restore_largest_block_data_len;

/**
 * Checks that each block a chain of data blocks was properly restored.
 *
 * @param last_block_entry      The entry corresponding to the last block in
 *                                  the chain.
 *
 * @return                      0 if the block chain is OK;
 *                              FS_ECORRUPT if corruption is detected;
 *                              nonzero on other error.
 */
static int
nffs_restore_validate_block_chain(struct nffs_hash_entry *last_block_entry)
{
    struct nffs_disk_block disk_block;
    struct nffs_hash_entry *cur;
    struct nffs_block block;
    uint32_t area_offset;
    uint8_t area_idx;
    int rc;

    cur = last_block_entry;

    while (cur != NULL) {
        nffs_flash_loc_expand(cur->nhe_flash_loc, &area_idx, &area_offset);

        rc = nffs_block_read_disk(area_idx, area_offset, &disk_block);
        if (rc != 0) {
            return rc;
        }

        rc = nffs_block_from_hash_entry(&block, cur);
        if (rc != 0) {
            return rc;
        }

        cur = block.nb_prev;
    }

    return 0;
}

static void
u32toa(char *dst, uint32_t val)
{
	uint8_t tmp;
	int idx = 0;
	int i;
	int print = 0;

	for (i = 0; i < 8; i++) {
		tmp = val >> 28;
		if (tmp || i == 7) {
			print = 1;
		}
		if (tmp < 10) {
			tmp += '0';
		} else {
			tmp += 'a' - 10;
		}
		if (print) {
			dst[idx++] = tmp;
		}
		val <<= 4;
	}
	dst[idx++] = '\0';
}

/**
 * If the specified inode entry is a dummy directory, this function moves
 * all its children to the lost+found directory.
 *
 * @param inode_entry           The parent inode to test and empty.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
nffs_restore_migrate_orphan_children(struct nffs_inode_entry *inode_entry)
{
    struct nffs_inode_entry *lost_found_sub;
    struct nffs_inode_entry *child_entry;
    char buf[32];
    int rc;

    if (!nffs_hash_id_is_dir(inode_entry->nie_hash_entry.nhe_id)) {
        /* Not a directory. */
        return 0;
    }

    if (inode_entry->nie_refcnt != 0) {
        /* Not a dummy. */
        return 0;
    }

    if (SLIST_EMPTY(&inode_entry->nie_child_list)) {
        /* No children to migrate. */
        return 0;
    }

    /* Create a directory in lost+found to hold the dummy directory's
     * contents.
     */
    strcpy(buf, "/lost+found/");
    u32toa(&buf[strlen(buf)], inode_entry->nie_hash_entry.nhe_id);

    rc = nffs_path_new_dir(buf, &lost_found_sub);
    if (rc != 0 && rc != FS_EEXIST) {
        return rc;
    }

    /* Move each child into the new subdirectory. */
    while ((child_entry = SLIST_FIRST(&inode_entry->nie_child_list)) != NULL) {
        rc = nffs_inode_rename(child_entry, lost_found_sub, NULL);
        if (rc != 0) {
            return rc;
        }
    }

    return 0;
}

static int
nffs_restore_should_sweep_inode_entry(struct nffs_inode_entry *inode_entry,
                                      int *out_should_sweep)
{
    struct nffs_inode inode;
    int rc;

    /* Determine if the inode is a dummy.  Dummy inodes have a reference count
     * of 0.  If it is a dummy, increment its reference count back to 1 so that
     * it can be properly deleted.  The presence of a dummy inode during the
     * final sweep step indicates file system corruption.  If the inode is a
     * directory, all its children should have been migrated to the /lost+found
     * directory prior to this.
     */
    if (inode_entry->nie_refcnt == 0) {
        *out_should_sweep = 1;
        inode_entry->nie_refcnt++;
        return 0;
    }

    /* Determine if the inode has been deleted.  If an inode has no parent (and
     * it isn't the root directory), it has been deleted from the disk and
     * should be swept from the RAM representation.
     */
    if (inode_entry->nie_hash_entry.nhe_id != NFFS_ID_ROOT_DIR) {
        rc = nffs_inode_from_entry(&inode, inode_entry);
        if (rc != 0) {
            *out_should_sweep = 0;
            return rc;
        }

        if (inode.ni_parent == NULL) {
            *out_should_sweep = 1;
            return 0;
        }
    }

    /* If this is a file inode, verify that all of its constituent blocks are
     * present.
     */
    if (nffs_hash_id_is_file(inode_entry->nie_hash_entry.nhe_id)) {
        rc = nffs_restore_validate_block_chain(
                inode_entry->nie_last_block_entry);
        if (rc == FS_ECORRUPT) {
            *out_should_sweep = 1;
            return 0;
        } else if (rc != 0) {
            *out_should_sweep = 0;
            return rc;
        }
    }

    /* This is a valid inode; don't sweep it. */
    *out_should_sweep = 0;
    return 0;
}

static void
nffs_restore_inode_from_dummy_entry(struct nffs_inode *out_inode,
                                    struct nffs_inode_entry *inode_entry)
{
    memset(out_inode, 0, sizeof *out_inode);
    out_inode->ni_inode_entry = inode_entry;
}

static int
nffs_restore_find_file_end_block(struct nffs_hash_entry *block_entry)
{
    struct nffs_inode_entry *inode_entry;
    struct nffs_block block;
    int rc;

    rc = nffs_block_from_hash_entry(&block, block_entry);
    assert(rc == 0);

    inode_entry = block.nb_inode_entry;

    /* Make sure the parent inode (file) points to the latest data block
     * restored so far.
     *
     * XXX: This is an O(n) operation (n = # of blocks in the file), and is
     * horribly inefficient for large files.  MYNEWT-161 has been opened to
     * address this.
     */
    if (inode_entry->nie_last_block_entry == NULL) {
        /* This is the first data block restored for this file. */
        inode_entry->nie_last_block_entry = block_entry;
        NFFS_LOG(DEBUG, "setting last block: %u\n", block_entry->nhe_id);
    } else {
        /* Determine if this this data block comes after our current idea of
         * the file's last data block.
         */

        rc = nffs_block_find_predecessor(
            block_entry, inode_entry->nie_last_block_entry->nhe_id);
        switch (rc) {
        case 0:
            /* The currently-last block is a predecessor of the new block; the
             * new block comes later.
             */
            NFFS_LOG(DEBUG, "replacing last block: %u --> %u\n",
                     inode_entry->nie_last_block_entry->nhe_id,
                     block_entry->nhe_id);
            inode_entry->nie_last_block_entry = block_entry;
            break;

        case FS_ENOENT:
            break;

        default:
            return rc;
        }
    }

    return 0;
}


static int
nffs_restore_find_file_ends(void)
{
    struct nffs_hash_entry *block_entry;
    struct nffs_hash_entry *next;
    int rc;
    int i;

    NFFS_HASH_FOREACH(block_entry, i, next) {
        if (!nffs_hash_id_is_inode(block_entry->nhe_id)) {
            rc = nffs_restore_find_file_end_block(block_entry);
            assert(rc == 0);
        }
    }

    return 0;
}

/**
 * Performs a sweep of the RAM representation at the end of a successful
 * restore.  The sweep phase performs the following actions of each inode in
 * the file system:
 *     1. If the inode is a dummy directory, its children are migrated to the
 *        lost+found directory.
 *     2. Else if the inode is a dummy file, it is fully deleted from RAM.
 *     3. Else, a CRC check is performed on each of the inode's constituent
 *        blocks.  If corruption is detected, the inode is fully deleted from
 *        RAM.
 *
 * @return                      0 on success; nonzero on failure.
 */
int
nffs_restore_sweep(void)
{
    struct nffs_inode_entry *inode_entry;
    struct nffs_hash_entry *entry;
    struct nffs_hash_entry *next;
    struct nffs_hash_list *list;
    struct nffs_inode inode;
    int del;
    int rc;
    int i;

    /* Iterate through every object in the hash table, deleting all inodes that
     * should be removed.
     */
    for (i = 0; i < NFFS_HASH_SIZE; i++) {
        list = nffs_hash + i;

        entry = SLIST_FIRST(list);
        while (entry != NULL) {
            next = SLIST_NEXT(entry, nhe_next);
            if (nffs_hash_id_is_inode(entry->nhe_id)) {
                inode_entry = (struct nffs_inode_entry *)entry;

                /* If this is a dummy inode directory, the file system is
                 * corrupted.  Move the directory's children inodes to the
                 * lost+found directory.
                 */
                rc = nffs_restore_migrate_orphan_children(inode_entry);
                if (rc != 0) {
                    return rc;
                }

                /* Determine if this inode needs to be deleted. */
                rc = nffs_restore_should_sweep_inode_entry(inode_entry, &del);
                if (rc != 0) {
                    return rc;
                }

                if (del) {
                    if (inode_entry->nie_hash_entry.nhe_flash_loc ==
                        NFFS_FLASH_LOC_NONE) {

                        nffs_restore_inode_from_dummy_entry(&inode,
                                                           inode_entry);
                    } else {
                        rc = nffs_inode_from_entry(&inode, inode_entry);
                        if (rc != 0) {
                            return rc;
                        }
                    }

                    /* Remove the inode and all its children from RAM.  We
                     * expect some file system corruption; the children are
                     * subject to garbage collection and may not exist in the
                     * hash.  Remove what is actually present and ignore
                     * corruption errors.
                     */
                    rc = nffs_inode_unlink_from_ram_corrupt_ok(&inode, &next);
                    if (rc != 0) {
                        return rc;
                    }
                    next = SLIST_FIRST(list);
                }
            }

            entry = next;
        }
    }

    return 0;
}

/**
 * Creates a dummy inode and inserts it into the hash table.  A dummy inode is
 * a temporary placeholder for a real inode that has not been restored yet.
 * These are necessary so that the inter-object links can be maintained until
 * the absent inode is eventually restored.  Dummy inodes are identified by a
 * reference count of 0.
 *
 * @param id                    The ID of the dummy inode to create.
 * @param out_inode_entry       On success, the dummy inode gets written here.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
nffs_restore_dummy_inode(uint32_t id,
                         struct nffs_inode_entry **out_inode_entry)
{
    struct nffs_inode_entry *inode_entry;

    inode_entry = nffs_inode_entry_alloc();
    if (inode_entry == NULL) {
        return FS_ENOMEM;
    }
    inode_entry->nie_hash_entry.nhe_id = id;
    inode_entry->nie_hash_entry.nhe_flash_loc = NFFS_FLASH_LOC_NONE;
    inode_entry->nie_refcnt = 0;

    nffs_hash_insert(&inode_entry->nie_hash_entry);

    *out_inode_entry = inode_entry;

    return 0;
}

/**
 * Determines if an already-restored inode should be replaced by another inode
 * just read from flash.  This function should only be called if both inodes
 * share the same ID.  The existing inode gets replaced if:
 *     o It is a dummy inode.
 *     o Its sequence number is less than that of the new inode.
 *
 * @param old_inode_entry       The already-restored inode to test.
 * @param disk_inode            The inode just read from flash.
 * @param out_should_replace    On success, 0=don't replace; 1=do replace.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
nffs_restore_inode_gets_replaced(struct nffs_inode_entry *old_inode_entry,
                                 const struct nffs_disk_inode *disk_inode,
                                 int *out_should_replace)
{
    struct nffs_inode old_inode;
    int rc;

    assert(old_inode_entry->nie_hash_entry.nhe_id == disk_inode->ndi_id);

    if (old_inode_entry->nie_refcnt == 0) {
        *out_should_replace = 1;
        return 0;
    }

    rc = nffs_inode_from_entry(&old_inode, old_inode_entry);
    if (rc != 0) {
        *out_should_replace = 0;
        return rc;
    }

    if (old_inode.ni_seq < disk_inode->ndi_seq) {
        *out_should_replace = 1;
        return 0;
    }

    if (old_inode.ni_seq == disk_inode->ndi_seq) {
        /* This is a duplicate of a previously-read inode.  This should never
         * happen.
         */
        *out_should_replace = 0;
        return FS_ECORRUPT;
    }

    *out_should_replace = 0;
    return 0;
}

/**
 * Determines if the specified inode should be added to the RAM representation
 * and adds it if appropriate.
 *
 * @param disk_inode            The inode just read from flash.
 * @param area_idx              The index of the area containing the inode.
 * @param area_offset           The offset within the area of the inode.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
nffs_restore_inode(const struct nffs_disk_inode *disk_inode, uint8_t area_idx,
                   uint32_t area_offset)
{
    struct nffs_inode_entry *inode_entry;
    struct nffs_inode_entry *parent;
    struct nffs_inode inode;
    int new_inode;
    int do_add;
    int rc;

    new_inode = 0;

    /* Check the inode's CRC.  If the inode is corrupt, discard it. */
    rc = nffs_crc_disk_inode_validate(disk_inode, area_idx, area_offset);
    if (rc != 0) {
        goto err;
    }

    inode_entry = nffs_hash_find_inode(disk_inode->ndi_id);
    if (inode_entry != NULL) {
        rc = nffs_restore_inode_gets_replaced(inode_entry, disk_inode,
                                              &do_add);
        if (rc != 0) {
            goto err;
        }

        if (do_add) {
            if (inode_entry->nie_hash_entry.nhe_flash_loc !=
                NFFS_FLASH_LOC_NONE) {

                rc = nffs_inode_from_entry(&inode, inode_entry);
                if (rc != 0) {
                    return rc;
                }
                if (inode.ni_parent != NULL) {
                    nffs_inode_remove_child(&inode);
                }
            }
 
            inode_entry->nie_hash_entry.nhe_flash_loc =
                nffs_flash_loc(area_idx, area_offset);
        }
    } else {
        inode_entry = nffs_inode_entry_alloc();
        if (inode_entry == NULL) {
            rc = FS_ENOMEM;
            goto err;
        }
        new_inode = 1;
        do_add = 1;

        inode_entry->nie_hash_entry.nhe_id = disk_inode->ndi_id;
        inode_entry->nie_hash_entry.nhe_flash_loc =
            nffs_flash_loc(area_idx, area_offset);

        nffs_hash_insert(&inode_entry->nie_hash_entry);
    }

    if (do_add) {
        inode_entry->nie_refcnt = 1;

        if (disk_inode->ndi_parent_id != NFFS_ID_NONE) {
            parent = nffs_hash_find_inode(disk_inode->ndi_parent_id);
            if (parent == NULL) {
                rc = nffs_restore_dummy_inode(disk_inode->ndi_parent_id,
                                             &parent);
                if (rc != 0) {
                    goto err;
                }
            }

            rc = nffs_inode_add_child(parent, inode_entry);
            if (rc != 0) {
                goto err;
            }
        } 


        if (inode_entry->nie_hash_entry.nhe_id == NFFS_ID_ROOT_DIR) {
            nffs_root_dir = inode_entry;
        }
    }

    if (nffs_hash_id_is_file(inode_entry->nie_hash_entry.nhe_id)) {
        NFFS_LOG(DEBUG, "restoring file; id=0x%08x\n",
                 inode_entry->nie_hash_entry.nhe_id);
        if (inode_entry->nie_hash_entry.nhe_id >= nffs_hash_next_file_id) {
            nffs_hash_next_file_id = inode_entry->nie_hash_entry.nhe_id + 1;
        }
    } else {
        NFFS_LOG(DEBUG, "restoring dir; id=0x%08x\n",
                 inode_entry->nie_hash_entry.nhe_id);
        if (inode_entry->nie_hash_entry.nhe_id >= nffs_hash_next_dir_id) {
            nffs_hash_next_dir_id = inode_entry->nie_hash_entry.nhe_id + 1;
        }
    }

    return 0;

err:
    if (new_inode) {
        nffs_inode_entry_free(inode_entry);
    }
    return rc;
}

/**
 * Indicates whether the specified data block is superseded by the just-read
 * disk data block.  A data block supersedes another if its ID is equal and its
 * sequence number is greater than that of the other block.
 *
 * @param out_should_replace    On success, 0 or 1 gets written here, to
 *                                  indicate whether replacement should occur.
 * @param old_block             The data block which has already been read and
 *                                  converted to its RAM representation.  This
 *                                  is the block that may be superseded.
 * @param disk_block            The disk data block that was just read from
 *                                  flash.  This is the block which may
 *                                  supersede the other.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
nffs_restore_block_gets_replaced(const struct nffs_block *old_block,
                                 const struct nffs_disk_block *disk_block,
                                 int *out_should_replace)
{
    assert(old_block->nb_hash_entry->nhe_id == disk_block->ndb_id);

    if (old_block->nb_seq < disk_block->ndb_seq) {
        *out_should_replace = 1;
        return 0;
    }

    if (old_block->nb_seq == disk_block->ndb_seq) {
        /* This is a duplicate of an previously-read inode.  This should never
         * happen.
         */
        return FS_ECORRUPT;
    }

    *out_should_replace = 0;
    return 0;
}

/**
 * Populates the nffs RAM state with the memory representation of the specified
 * disk data block.
 *
 * @param disk_block            The source disk block to insert.
 * @param area_idx              The ID of the area containing the block.
 * @param area_offset           The area_offset within the area of the block.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
nffs_restore_block(const struct nffs_disk_block *disk_block, uint8_t area_idx,
                   uint32_t area_offset)
{
    struct nffs_inode_entry *inode_entry;
    struct nffs_hash_entry *entry;
    struct nffs_block block;
    int do_replace;
    int new_block;
    int rc;

    new_block = 0;

    /* Check the block's CRC.  If the block is corrupt, discard it.  If this
     * block would have superseded another, the old block becomes current.
     */
    rc = nffs_crc_disk_block_validate(disk_block, area_idx, area_offset);
    if (rc != 0) {
        goto err;
    }

    entry = nffs_hash_find_block(disk_block->ndb_id);
    if (entry != NULL) {
        rc = nffs_block_from_hash_entry_no_ptrs(&block, entry);
        if (rc != 0) {
            goto err;
        }

        rc = nffs_restore_block_gets_replaced(&block, disk_block, &do_replace);
        if (rc != 0) {
            goto err;
        }

        if (!do_replace) {
            /* The new block is superseded by the old; nothing to do. */
            return 0;
        }

        nffs_block_delete_from_ram(entry);
    }

    entry = nffs_block_entry_alloc();
    if (entry == NULL) {
        rc = FS_ENOMEM;
        goto err;
    }
    new_block = 1;
    entry->nhe_id = disk_block->ndb_id;
    entry->nhe_flash_loc = nffs_flash_loc(area_idx, area_offset);

    /* The block is ready to be inserted into the hash. */

    nffs_hash_insert(entry);

    if (disk_block->ndb_id >= nffs_hash_next_block_id) {
        nffs_hash_next_block_id = disk_block->ndb_id + 1;
    }

    /* Make sure the maximum block data size is not set lower than the size of
     * an existing block.
     */
    if (disk_block->ndb_data_len > nffs_restore_largest_block_data_len) {
        nffs_restore_largest_block_data_len = disk_block->ndb_data_len;
    }

    NFFS_LOG(DEBUG, "restoring block; id=0x%08x seq=%u inode_id=%u prev_id=%u "
             "data_len=%u\n", disk_block->ndb_id, disk_block->ndb_seq,
             disk_block->ndb_inode_id, disk_block->ndb_prev_id,
             disk_block->ndb_data_len);

    inode_entry = nffs_hash_find_inode(disk_block->ndb_inode_id);
    if (inode_entry == NULL) {
        rc = nffs_restore_dummy_inode(disk_block->ndb_inode_id, &inode_entry);
        if (rc != 0) {
            goto err;
        }
    }


    return 0;

err:
    if (new_block) {
        nffs_block_entry_free(entry);
    }
    return rc;
}

/**
 * Populates the nffs RAM state with the memory representation of the specified
 * disk object.
 *
 * @param disk_object           The source disk object to convert.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
nffs_restore_object(const struct nffs_disk_object *disk_object)
{
    int rc;

    switch (disk_object->ndo_type) {
    case NFFS_OBJECT_TYPE_INODE:
        rc = nffs_restore_inode(&disk_object->ndo_disk_inode,
                                disk_object->ndo_area_idx,
                                disk_object->ndo_offset);
        break;

    case NFFS_OBJECT_TYPE_BLOCK:
        rc = nffs_restore_block(&disk_object->ndo_disk_block,
                                disk_object->ndo_area_idx,
                                disk_object->ndo_offset);
        break;

    default:
        assert(0);
        rc = FS_EINVAL;
        break;
    }

    return rc;
}

/**
 * Reads a single disk object from flash.
 *
 * @param area_idx              The area to read the object from.
 * @param area_offset           The offset within the area to read from.
 * @param out_disk_object       On success, the restored object gets written
 *                                  here.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
nffs_restore_disk_object(int area_idx, uint32_t area_offset,
                         struct nffs_disk_object *out_disk_object)
{
    uint32_t magic;
    int rc;

    rc = nffs_flash_read(area_idx, area_offset, &magic, sizeof magic);
    if (rc != 0) {
        return rc;
    }

    switch (magic) {
    case NFFS_INODE_MAGIC:
        out_disk_object->ndo_type = NFFS_OBJECT_TYPE_INODE;
        rc = nffs_inode_read_disk(area_idx, area_offset,
                                 &out_disk_object->ndo_disk_inode);
        break;

    case NFFS_BLOCK_MAGIC:
        out_disk_object->ndo_type = NFFS_OBJECT_TYPE_BLOCK;
        rc = nffs_block_read_disk(area_idx, area_offset,
                                 &out_disk_object->ndo_disk_block);
        break;

    case 0xffffffff:
        rc = FS_EEMPTY;
        break;

    default:
        rc = FS_ECORRUPT;
        break;
    }

    if (rc != 0) {
        return rc;
    }

    out_disk_object->ndo_area_idx = area_idx;
    out_disk_object->ndo_offset = area_offset;

    return 0;
}

/**
 * Calculates the disk space occupied by the specified disk object.
 *
 * @param disk_object
 */
static int
nffs_restore_disk_object_size(const struct nffs_disk_object *disk_object)
{
    switch (disk_object->ndo_type) {
    case NFFS_OBJECT_TYPE_INODE:
        return sizeof disk_object->ndo_disk_inode +
                      disk_object->ndo_disk_inode.ndi_filename_len;

    case NFFS_OBJECT_TYPE_BLOCK:
        return sizeof disk_object->ndo_disk_block +
                      disk_object->ndo_disk_block.ndb_data_len;

    default:
        assert(0);
        return 1;
    }
}

/**
 * Reads the specified area from disk and loads its contents into the RAM
 * representation.
 *
 * @param area_idx              The index of the area to read.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
nffs_restore_area_contents(int area_idx)
{
    struct nffs_disk_object disk_object;
    struct nffs_area *area;
    int rc;

    area = nffs_areas + area_idx;

    area->na_cur = sizeof (struct nffs_disk_area);
    while (1) {
        rc = nffs_restore_disk_object(area_idx, area->na_cur,  &disk_object);
        switch (rc) {
        case 0:
            /* Valid object; restore it into the RAM representation. */
            nffs_restore_object(&disk_object);
            area->na_cur += nffs_restore_disk_object_size(&disk_object);
            break;

        case FS_ECORRUPT:
            /* Invalid object; keep scanning for a valid magic number. */
            area->na_cur++;
            break;

        case FS_EEMPTY:
        case FS_EOFFSET:
            /* End of disk encountered; area fully restored. */
            return 0;

        default:
            return rc;
        }
    }
}

/**
 * Reads and parses one area header.  This function does not read the area's
 * contents.
 *
 * @param out_is_scratch        On success, 0 or 1 gets written here,
 *                                  indicating whether the area is a scratch
 *                                  area.
 * @param area_offset           The flash offset of the start of the area.
 *
 * @return                      0 on success;
 *                              nonzero on failure.
 */
static int
nffs_restore_detect_one_area(uint8_t flash_id, uint32_t area_offset,
                             struct nffs_disk_area *out_disk_area)
{
    int rc;

    rc = hal_flash_read(flash_id, area_offset, out_disk_area,
                        sizeof *out_disk_area);
    if (rc != 0) {
        return FS_EHW;
    }

    if (!nffs_area_magic_is_set(out_disk_area)) {
        return FS_ECORRUPT;
    }

    return 0;
}

/**
 * Repairs the effects of a corrupt scratch area.  Scratch area corruption can
 * occur when the system resets while a garbage collection cycle is in
 * progress.
 *
 * @return                      0 on success; nonzero on failure.
 */
static int
nffs_restore_corrupt_scratch(void)
{
    struct nffs_inode_entry *inode_entry;
    struct nffs_hash_entry *entry;
    struct nffs_hash_entry *next;
    uint32_t area_offset;
    uint16_t good_idx;
    uint16_t bad_idx;
    uint8_t area_idx;
    int rc;
    int i;

    /* Search for a pair of areas with identical IDs.  If found, these areas
     * represent the source and destination areas of a garbage collection
     * cycle.  The shorter of the two areas was the destination area.  Since
     * the garbage collection cycle did not finish, the source area contains a
     * more complete set of objects than the destination area.
     *
     * good_idx = index of source area.
     * bad_idx  = index of destination area; this will be turned into the
     *            scratch area.
     */
    rc = nffs_area_find_corrupt_scratch(&good_idx, &bad_idx);
    if (rc != 0) {
        return rc;
    }

    /* Invalidate all objects resident in the bad area. */
    for (i = 0; i < NFFS_HASH_SIZE; i++) {
        entry = SLIST_FIRST(&nffs_hash[i]);
        while (entry != NULL) {
            next = SLIST_NEXT(entry, nhe_next);

            nffs_flash_loc_expand(entry->nhe_flash_loc,
                                 &area_idx, &area_offset);
            if (area_idx == bad_idx) {
                if (nffs_hash_id_is_block(entry->nhe_id)) {
                    rc = nffs_block_delete_from_ram(entry);
                    if (rc != 0) {
                        return rc;
                    }
                } else {
                    inode_entry = (struct nffs_inode_entry *)entry;
                    inode_entry->nie_refcnt = 0;
                }
            }

            entry = next;
        }
    }

    /* Now that the objects in the scratch area have been invalidated, reload
     * everything from the good area.
     */
    rc = nffs_restore_area_contents(good_idx);
    if (rc != 0) {
        return rc;
    }

    /* Convert the bad area into a scratch area. */
    rc = nffs_format_area(bad_idx, 1);
    if (rc != 0) {
        return rc;
    }
    nffs_scratch_area_idx = bad_idx;

    return 0;
}

static void
nffs_log_contents(void)
{
#if LOG_LEVEL > LOG_LEVEL_DEBUG
    return;
#endif

    struct nffs_inode_entry *inode_entry;
    struct nffs_hash_entry *entry;
    struct nffs_hash_entry *next;
    struct nffs_block block;
    struct nffs_inode inode;
    int rc;
    int i;

    NFFS_HASH_FOREACH(entry, i, next) {
        if (nffs_hash_id_is_block(entry->nhe_id)) {
            rc = nffs_block_from_hash_entry(&block, entry);
            assert(rc == 0);
            NFFS_LOG(DEBUG, "block; id=%u inode_id=", entry->nhe_id);
            if (block.nb_inode_entry == NULL) {
                NFFS_LOG(DEBUG, "null ");
            } else {
                NFFS_LOG(DEBUG, "%u ",
                         block.nb_inode_entry->nie_hash_entry.nhe_id);
            }

            NFFS_LOG(DEBUG, "prev_id=");
            if (block.nb_prev == NULL) {
                NFFS_LOG(DEBUG, "null ");
            } else {
                NFFS_LOG(DEBUG, "%u ", block.nb_prev->nhe_id);
            }

            NFFS_LOG(DEBUG, "data_len=%u\n", block.nb_data_len);
        } else {
            inode_entry = (void *)entry;
            rc = nffs_inode_from_entry(&inode, inode_entry);
            assert(rc == 0);

            if (nffs_hash_id_is_file(entry->nhe_id)) {
                NFFS_LOG(DEBUG, "file; id=%u name=%.3s block_id=",
                         entry->nhe_id, inode.ni_filename);
                if (inode_entry->nie_last_block_entry == NULL) {
                    NFFS_LOG(DEBUG, "null");
                } else {
                    NFFS_LOG(DEBUG, "%u",
                             inode_entry->nie_last_block_entry->nhe_id);
                }
                NFFS_LOG(DEBUG, "\n");
            } else {
                inode_entry = (void *)entry;
                NFFS_LOG(DEBUG, "dir; id=%u name=%.3s\n", entry->nhe_id,
                         inode.ni_filename);

            }
        }
    }
}

/**
 * Searches for a valid nffs file system among the specified areas.  This
 * function succeeds if a file system is detected among any subset of the
 * supplied areas.  If the area set does not contain a valid file system,
 * a new one can be created via a call to nffs_format().
 *
 * @param area_descs        The area set to search.  This array must be
 *                              terminated with a 0-length area.
 *
 * @return                  0 on success;
 *                          FS_ECORRUPT if no valid file system was detected;
 *                          other nonzero on error.
 */
int
nffs_restore_full(const struct nffs_area_desc *area_descs)
{
    struct nffs_disk_area disk_area;
    int cur_area_idx;
    int use_area;
    int rc;
    int i;

    /* Start from a clean state. */
    rc = nffs_misc_reset();
    if (rc) {
        return rc;
    }
    nffs_restore_largest_block_data_len = 0;

    /* Read each area from flash. */
    for (i = 0; area_descs[i].nad_length != 0; i++) {
        if (i > NFFS_MAX_AREAS) {
            rc = FS_EINVAL;
            goto err;
        }

        rc = nffs_restore_detect_one_area(area_descs[i].nad_flash_id,
                                          area_descs[i].nad_offset,
                                          &disk_area);
        switch (rc) {
        case 0:
            use_area = 1;
            break;

        case FS_ECORRUPT:
            use_area = 0;
            break;

        default:
            goto err;
        }

        if (use_area) {
            if (disk_area.nda_id == NFFS_AREA_ID_NONE &&
                nffs_scratch_area_idx != NFFS_AREA_ID_NONE) {

                /* Don't allow more than one scratch area. */
                use_area = 0;
            }
        }

        if (use_area) {
            /* Populate RAM with a representation of this area. */
            cur_area_idx = nffs_num_areas;

            rc = nffs_misc_set_num_areas(nffs_num_areas + 1);
            if (rc != 0) {
                goto err;
            }

            nffs_areas[cur_area_idx].na_offset = area_descs[i].nad_offset;
            nffs_areas[cur_area_idx].na_length = area_descs[i].nad_length;
            nffs_areas[cur_area_idx].na_flash_id = area_descs[i].nad_flash_id;
            nffs_areas[cur_area_idx].na_gc_seq = disk_area.nda_gc_seq;
            nffs_areas[cur_area_idx].na_id = disk_area.nda_id;

            if (disk_area.nda_id == NFFS_AREA_ID_NONE) {
                nffs_areas[cur_area_idx].na_cur = NFFS_AREA_OFFSET_ID;
                nffs_scratch_area_idx = cur_area_idx;
            } else {
                nffs_areas[cur_area_idx].na_cur =
                    sizeof (struct nffs_disk_area);
                nffs_restore_area_contents(cur_area_idx);
            }
        }
    }

    /* All areas have been restored from flash. */

    if (nffs_scratch_area_idx == NFFS_AREA_ID_NONE) {
        /* No scratch area.  The system may have been rebooted in the middle of
         * a garbage collection cycle.  Look for a candidate scratch area.
         */
        rc = nffs_restore_corrupt_scratch();
        if (rc != 0) {
            if (rc == FS_ENOENT) {
                rc = FS_ECORRUPT;
            }
            goto err;
        }
    }

    /* Ensure this file system contains a valid scratch area. */
    rc = nffs_misc_validate_scratch();
    if (rc != 0) {
        goto err;
    }

    /* Make sure the file system contains a valid root directory. */
    rc = nffs_misc_validate_root_dir();
    if (rc != 0) {
        goto err;
    }

    /* Ensure there is a "/lost+found" directory. */
    rc = nffs_misc_create_lost_found_dir();
    if (rc != 0) {
        goto err;
    }

    /* Find the last block in each file inode. */
    nffs_restore_find_file_ends();

    /* Delete from RAM any objects that were invalidated when subsequent areas
     * were restored.
     */
    nffs_restore_sweep();

    /* Set the maximum data block size according to the size of the smallest
     * area.
     */
    rc = nffs_misc_set_max_block_data_len(nffs_restore_largest_block_data_len);
    if (rc != 0) {
        goto err;
    }

    NFFS_LOG(DEBUG, "CONTENTS\n");
    nffs_log_contents();

    return 0;

err:
    nffs_misc_reset();
    return rc;
}
