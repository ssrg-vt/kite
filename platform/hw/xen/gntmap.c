/*
 * Manages grant mappings from other domains.
 *
 * Diego Ongaro <diego.ongaro@citrix.com>, July 2008
 * Ruslan Nikolaev <rnikola@vt.edu>, July 2018
 *
 * Files of type FTYPE_GNTMAP contain a gntmap, which is an array of
 * (host address, grant handle) pairs. Grant handles come from a hypervisor map
 * operation and are needed for the corresponding unmap.
 *
 * This is a rather naive implementation in terms of performance. If we start
 * using it frequently, there's definitely some low-hanging fruit here.
 *
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
 * DEALINGS IN THE SOFTWARE.
 */

#include <mini-os/os.h>
#include <mini-os/lib.h>
#include <xen/grant_table.h>
#include <mini-os/gntmap.h>
#include <mini-os/machine/pcpu.h>

#include <bmk-core/errno.h>
#include <bmk-core/string.h>
#include <bmk-core/memalloc.h>
#include <bmk-core/pgalloc.h>
#include <bmk-core/printf.h>
#include <bmk-core/platform.h>

#define DEFAULT_MAX_GRANTS 4096

struct gntmap_entry {
    unsigned long host_addr;
    grant_handle_t handle;
};

static inline int
gntmap_entry_used(struct gntmap_entry *entry)
{
    return __atomic_load_n(&entry->host_addr, __ATOMIC_SEQ_CST) != 0;
}

static struct gntmap_entry*
gntmap_find_free_entry(struct gntmap *map)
{
    int i;

    for (i = 0; i < map->nentries; i++) {
        unsigned long free_addr = 0;

        /* Mark the entry as used. */
	if (!gntmap_entry_used(&map->entries[i]) &&
                __atomic_compare_exchange_n(&map->entries[i].host_addr,
                    &free_addr, (unsigned long) -1L, 0, __ATOMIC_SEQ_CST,
                    __ATOMIC_SEQ_CST))
            return &map->entries[i];
    }

#ifdef GNTMAP_DEBUG
    bmk_printf("gntmap_find_free_entry(map=%p): all %d entries full\n",
           map, map->nentries);
#endif
    return NULL;
}

static struct gntmap_entry*
gntmap_find_entry(struct gntmap *map, unsigned long addr)
{
    int i;

    for (i = 0; i < map->nentries; i++) {
        if (__atomic_load_n(&map->entries[i].host_addr, __ATOMIC_SEQ_CST)
                == addr)
            return &map->entries[i];
    }

    return NULL;
}

static void
_gntmap_set_max_grants(struct gntmap *map, int count)
{
    struct gntmap_entry *entries;

#ifdef GNTMAP_DEBUG
    bmk_printf("gntmap_set_max_grants(map=%p, count=%d)\n", map, count);
#endif

    entries = bmk_memcalloc(count,
	sizeof(struct gntmap_entry), BMK_MEMWHO_WIREDBMK);
    if (entries == NULL)
        bmk_platform_halt("cannot allocate gntmap");

    map->entries = entries;
    map->nentries = count;
}

static int
_gntmap_map_grant_ref(struct gntmap_entry *entry,
                      unsigned long host_addr,
                      uint32_t domid,
                      uint32_t ref,
                      int writable)
{
    struct gnttab_map_grant_ref op;
    int rc;

    op.ref = (grant_ref_t) ref;
    op.dom = (domid_t) domid;
    op.host_addr = (uint64_t) host_addr;
    op.flags = GNTMAP_host_map;
    if (!writable)
        op.flags |= GNTMAP_readonly;

    rc = HYPERVISOR_grant_table_op(GNTTABOP_map_grant_ref, &op, 1);
    if (rc != 0 || op.status != GNTST_okay) {
        bmk_printf("GNTTABOP_map_grant_ref failed: "
               "returned %d, status %d\n",
               rc, op.status);
        __atomic_store_n(&entry->host_addr, 0, __ATOMIC_SEQ_CST);
	return rc != 0 ? rc : op.status;
    }

    __atomic_store_n(&entry->host_addr, host_addr, __ATOMIC_SEQ_CST);
    entry->handle = op.handle;
    return 0;
}

static int
_gntmap_unmap_grant_ref(struct gntmap_entry *entry)
{
    struct gnttab_unmap_grant_ref op;
    int rc;

    op.host_addr    = (uint64_t) entry->host_addr;
    op.dev_bus_addr = 0;
    op.handle       = entry->handle;

    rc = HYPERVISOR_grant_table_op(GNTTABOP_unmap_grant_ref, &op, 1);
    if (rc != 0 || op.status != GNTST_okay) {
        bmk_printf("GNTTABOP_unmap_grant_ref failed: "
               "returned %d, status %d\n",
               rc, op.status);
        return rc != 0 ? rc : op.status;
    }

    __atomic_store_n(&entry->host_addr, 0, __ATOMIC_SEQ_CST);
    return 0;
}

static int
_gntmap_munmap(struct gntmap *map, unsigned long start_address, int count)
{
    int i, rc;
    struct gntmap_entry *ent;

#ifdef GNTMAP_DEBUG
    bmk_printf("gntmap_munmap(map=%p, start_address=%lx, count=%d)\n",
           map, start_address, count);
#endif

    for (i = 0; i < count; i++) {
        ent = gntmap_find_entry(map, start_address + PAGE_SIZE * i);
        if (ent == NULL) {
            bmk_printf("gntmap: tried to munmap unknown page\n");
            return -BMK_EINVAL;
        }

        rc = _gntmap_unmap_grant_ref(ent);
        if (rc != 0)
            return rc;
    }

    return 0;
}

int
gntmap_munmap(struct gntmap *map, unsigned long start_address, int count)
{
    int rc = _gntmap_munmap(map, start_address, count);
// FIXME: need to check this
//    bmk_pgfree((void *) start_address, gntmap_map2order((unsigned) count));
    return rc;
}

void*
gntmap_map_grant_refs(struct gntmap *map,
                      uint32_t count,
                      uint32_t *domids,
                      int domids_stride,
                      uint32_t *refs,
                      int writable)
{
    unsigned long addr;
    struct gntmap_entry *ent;
    int i;

#ifdef GNTMAP_DEBUG
    bmk_printf("gntmap_map_grant_refs(map=%p, count=%u, "
           "domids=%p [%u...], domids_stride=%d, "
           "refs=%p [%u...], writable=%d)\n",
           map, count,
           domids, domids == NULL ? 0 : domids[0], domids_stride,
           refs, refs == NULL ? 0 : refs[0], writable);
#endif

    addr = (unsigned long) bmk_pgalloc_align(gntmap_map2order(count),
                               BMK_PCPU_PAGE_SIZE);
    if (addr == 0)
        return NULL;

    for (i = 0; i < count; i++) {
        ent = gntmap_find_free_entry(map);
        if (ent == NULL ||
            _gntmap_map_grant_ref(ent,
                                  addr + PAGE_SIZE * i,
                                  domids[i * domids_stride],
                                  refs[i],
                                  writable) != 0) {

            (void) _gntmap_munmap(map, addr, i);
            bmk_pgfree((void *) addr, gntmap_map2order(count));
            return NULL;
        }
    }

    return (void*) addr;
}

void
gntmap_init(struct gntmap *map)
{
#ifdef GNTMAP_DEBUG
    bmk_printf("gntmap_init(map=%p)\n", map);
#endif
    _gntmap_set_max_grants(map, DEFAULT_MAX_GRANTS);
}

void
gntmap_fini(struct gntmap *map)
{
    struct gntmap_entry *ent;
    int i;

#ifdef GNTMAP_DEBUG
    bmk_printf("gntmap_fini(map=%p)\n", map);
#endif

    for (i = 0; i < map->nentries; i++) {
        ent = &map->entries[i];
        if (gntmap_entry_used(ent))
            (void) _gntmap_unmap_grant_ref(ent);
    }

    bmk_memfree(map->entries, BMK_MEMWHO_WIREDBMK);
    map->entries = NULL;
    map->nentries = 0;
}
