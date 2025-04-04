/*
 * YAFFS: Yet Another Flash File System. A NAND-flash specific file system.
 *
 * Copyright (C) 2002-2018 Aleph One Ltd.
 *
 * Created by Charles Manning <charles@aleph1.co.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "yaffs_cache.h"

/*------------------------ Short Operations Cache ------------------------------
 *   In many situations where there is no high level buffering  a lot of
 *   reads might be short sequential reads, and a lot of writes may be short
 *   sequential writes. eg. scanning/writing a jpeg file.
 *   In these cases, a short read/write cache can provide a huge perfomance
 *   benefit with dumb-as-a-rock code.
 *   In Linux, the page cache provides read buffering and the short op cache
 *   provides write buffering.
 *
 *   There are a small number (~10) of cache chunks per device so that we don't
 *   need a very intelligent search.
 */

int yaffs_obj_cache_dirty(struct yaffs_obj *obj)
{
    struct yaffs_dev *dev = obj->my_dev;
    int i;
    struct yaffs_cache_manager *mgr = &dev->cache_mgr;

    for (i = 0; i < mgr->n_caches; i++)
    {
        struct yaffs_cache *cache = &mgr->cache[i];

        if (cache->object == obj && cache->dirty)
            return 1;
    }

    return 0;
}

void yaffs_flush_single_cache(struct yaffs_cache *cache, int discard)
{

    if (!cache || cache->locked)
        return;

    /* Write it out and free it up  if need be.*/
    if (cache->dirty)
    {
        yaffs_wr_data_obj(cache->object,
                          cache->chunk_id,
                          cache->data,
                          cache->n_bytes,
                          1);

        cache->dirty = 0;
    }

    if (discard)
        cache->object = NULL;
}

void yaffs_flush_file_cache(struct yaffs_obj *obj, int discard)
{
    struct yaffs_dev *dev = obj->my_dev;
    int i;
    struct yaffs_cache_manager *mgr = &dev->cache_mgr;

    if (mgr->n_caches < 1)
        return;


    /* Find the chunks for this object and flush them. */
    for (i = 0; i < mgr->n_caches; i++)
    {
        struct yaffs_cache *cache = &mgr->cache[i];

        if (cache->object == obj)
            yaffs_flush_single_cache(cache, discard);
    }

}


void yaffs_flush_whole_cache(struct yaffs_dev *dev, int discard)
{
    struct yaffs_cache_manager *mgr = &dev->cache_mgr;
    struct yaffs_obj *obj;
    int i;

    /* Find a dirty object in the cache and flush it...
     * until there are no further dirty objects.
     */
    do
    {
        obj = NULL;
        for (i = 0; i < mgr->n_caches && !obj; i++)
        {
            struct yaffs_cache *cache = &mgr->cache[i];
            if (cache->object && cache->dirty)
                obj = cache->object;
        }
        if (obj)
            yaffs_flush_file_cache(obj, discard);
    }
    while (obj);

}

/* Grab us an unused cache chunk for use.
 * First look for an empty one.
 * Then look for the least recently used non-dirty one.
 * Then look for the least recently used dirty one...., flush and look again.
 */
static struct yaffs_cache *yaffs_grab_chunk_worker(struct yaffs_dev *dev)
{
    struct yaffs_cache_manager *mgr = &dev->cache_mgr;
    int i;

    for (i = 0; i < mgr->n_caches; i++)
    {
        struct yaffs_cache *cache = &mgr->cache[i];
        if (!cache->object)
            return cache;
    }

    return NULL;
}

struct yaffs_cache *yaffs_grab_chunk_cache(struct yaffs_dev *dev)
{
    struct yaffs_cache_manager *mgr = &dev->cache_mgr;
    struct yaffs_cache *cache;
    int usage;
    int i;

    if (mgr->n_caches < 1)
        return NULL;

    /* First look for an unused cache */

    cache = yaffs_grab_chunk_worker(dev);

    if (cache)
        return cache;

    /*
     * Thery were all in use.
     * Find the LRU cache and flush it if it is dirty.
     */

    usage = -1;
    cache = NULL;

    for (i = 0; i < mgr->n_caches; i++)
    {
        struct yaffs_cache *this_cache = &mgr->cache[i];

        if (this_cache->object &&
                !this_cache->locked &&
                (this_cache->last_use < usage || !cache))
        {
            usage = this_cache->last_use;
            cache = this_cache;
        }
    }

#if 1
    yaffs_flush_single_cache(cache, 1);
#else
    yaffs_flush_file_cache(cache->object, 1);
    cache = yaffs_grab_chunk_worker(dev);
#endif

    return cache;
}

/* Find a cached chunk */
struct yaffs_cache *yaffs_find_chunk_cache(const struct yaffs_obj *obj,
        int chunk_id)
{
    struct yaffs_dev *dev = obj->my_dev;
    struct yaffs_cache_manager *mgr = &dev->cache_mgr;
    int i;

    if (mgr->n_caches < 1)
        return NULL;

    for (i = 0; i < mgr->n_caches; i++)
    {
        struct yaffs_cache *cache = &mgr->cache[i];

        if (cache->object == obj &&
                cache->chunk_id == chunk_id)
        {
            dev->cache_hits++;
            return cache;
        }
    }
    return NULL;
}

/* Mark the chunk for the least recently used algorithym */
void yaffs_use_cache(struct yaffs_dev *dev, struct yaffs_cache *cache,
                     int is_write)
{
    struct yaffs_cache_manager *mgr = &dev->cache_mgr;
    int i;

    if (mgr->n_caches < 1)
        return;

    if (mgr->cache_last_use < 0 ||
            mgr->cache_last_use > 100000000)
    {
        /* Reset the cache usages */
        for (i = 1; i < mgr->n_caches; i++)
            mgr->cache[i].last_use = 0;

        mgr->cache_last_use = 0;
    }
    mgr->cache_last_use++;
    cache->last_use = mgr->cache_last_use;

    if (is_write)
        cache->dirty = 1;
}

/* Invalidate a single cache page.
 * Do this when a whole page gets written,
 * ie the short cache for this page is no longer valid.
 */
void yaffs_invalidate_chunk_cache(struct yaffs_obj *object, int chunk_id)
{
    struct yaffs_cache *cache;

    cache = yaffs_find_chunk_cache(object, chunk_id);
    if (cache)
        cache->object = NULL;
}

/* Invalidate all the cache pages associated with this object
 * Do this whenever the file is deleted or resized.
 */
void yaffs_invalidate_file_cache(struct yaffs_obj *in)
{
    int i;
    struct yaffs_dev *dev = in->my_dev;
    struct yaffs_cache_manager *mgr = &dev->cache_mgr;

    /* Invalidate it. */
    for (i = 0; i < mgr->n_caches; i++)
    {
        struct yaffs_cache *cache = &mgr->cache[i];

        if (cache->object == in)
            cache->object = NULL;
    }
}

int yaffs_count_dirty_caches(struct yaffs_dev *dev)
{
    int n_dirty;
    int i;
    struct yaffs_cache_manager *mgr = &dev->cache_mgr;

    for (n_dirty = 0, i = 0; i < mgr->n_caches; i++)
    {
        if (mgr->cache[i].dirty)
            n_dirty++;
    }

    return n_dirty;
}

int yaffs_cache_init(struct yaffs_dev *dev)
{
    struct yaffs_cache_manager *mgr = &dev->cache_mgr;
    int init_failed = 0;

    if (dev->param.n_caches > YAFFS_MAX_SHORT_OP_CACHES)
        dev->param.n_caches = YAFFS_MAX_SHORT_OP_CACHES;

    mgr->n_caches = dev->param.n_caches;
    if (mgr->n_caches > 0)
    {
        int i;
        void *buf;
        u32 cache_bytes =
            mgr->n_caches * sizeof(struct yaffs_cache);



        mgr->cache = kmalloc(cache_bytes, GFP_NOFS);

        buf = (u8 *) mgr->cache;

        if (mgr->cache)
            memset(mgr->cache, 0, cache_bytes);

        for (i = 0; i < mgr->n_caches && buf; i++)
        {
            struct yaffs_cache *cache = &mgr->cache[i];

            cache->object = NULL;
            cache->last_use = 0;
            cache->dirty = 0;
            cache->data = buf =
                              kmalloc(dev->param.total_bytes_per_chunk, GFP_NOFS);
        }
        if (!buf)
            init_failed = 1;

        mgr->cache_last_use = 0;
    }

    return init_failed ? -1 : 0;
}

void yaffs_cache_deinit(struct yaffs_dev *dev)
{
    struct yaffs_cache_manager *mgr = &dev->cache_mgr;
    int i;

    if (mgr->n_caches < 1 || !mgr->cache)
        return;

    for (i = 0; i < mgr->n_caches; i++)
    {

        struct yaffs_cache *cache = &mgr->cache[i];
        kfree(cache->data);
        cache->data = NULL;
    }

    kfree(mgr->cache);
    mgr->cache = NULL;
}
