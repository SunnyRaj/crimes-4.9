#include <assert.h>
#include <libxl.h>
#include "xc_sr_common.h"
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <assert.h>
#include <arpa/inet.h>

#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>

#include <fcntl.h>
#include <sys/stat.h>

#include <libvmi/libvmi.h>
#include "xc_pipe.h"

/* Backup VM memcpy related variables */
unsigned nr_end_checkpoint = 0;

/* LibVMI related variables */
//#define ENABLE_LIBVMI  //comment to disable VMI
int counter = 1;
int buf;
int xen_write_fd = 0;             //Linux Pipe 1
int xen_read_fd = 0;            //Linux Pipe 2

struct vmi_requirements vmi_req;

/* Timer related variables */
typedef long long NANOSECONDS;
typedef struct timespec TIMESPEC;

static inline NANOSECONDS ns_timer(void)
{
    TIMESPEC curr_time;
    clock_gettime(CLOCK_MONOTONIC, &curr_time);
    return (NANOSECONDS) (curr_time.tv_sec * 1000000000LL) +
        (NANOSECONDS) (curr_time.tv_nsec);
}

/* Writes an Image header and Domain header into the stream.
 */
static int write_headers(struct xc_sr_context *ctx, uint16_t guest_type)
{
    xc_interface *xch = ctx->xch;
    int32_t xen_version = xc_version(xch, XENVER_version, NULL);
    struct xc_sr_ihdr ihdr =
        {
            .marker  = IHDR_MARKER,
            .id      = htonl(IHDR_ID),
            .version = htonl(IHDR_VERSION),
            .options = htons(IHDR_OPT_LITTLE_ENDIAN),
        };
    struct xc_sr_dhdr dhdr =
        {
            .type       = guest_type,
            .page_shift = XC_PAGE_SHIFT,
            .xen_major  = (xen_version >> 16) & 0xffff,
            .xen_minor  = (xen_version)       & 0xffff,
        };

    if ( xen_version < 0 )
    {
        PERROR("Unable to obtain Xen Version");
        return -1;
    }

    if ( write_exact(ctx->fd, &ihdr, sizeof(ihdr)) )
    {
        PERROR("Unable to write Image Header to stream");
        return -1;
    }

    if ( write_exact(ctx->fd, &dhdr, sizeof(dhdr)) )
    {
        PERROR("Unable to write Domain Header to stream");
        return -1;
    }

    return 0;
}

/*
 * Writes an END record into the stream.
 */

static int write_end_record(struct xc_sr_context *ctx)
{
    struct xc_sr_record end = { REC_TYPE_END, 0, NULL };

    return write_record(ctx, &end);
}

/*
 * Writes a CHECKPOINT record into the stream.
 */
static int write_checkpoint_record(struct xc_sr_context *ctx)
{
    struct xc_sr_record checkpoint = { REC_TYPE_CHECKPOINT, 0, NULL };

    return write_record(ctx, &checkpoint);
}

static int map_primary_and_backup(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    xen_pfn_t *mfns = NULL;
    xen_pfn_t pfn;
    int *errors = NULL, *bckp_errors = NULL;
    unsigned i;
    int rc = 0;

    unsigned nr_pfns = ctx->save.p2m_size;

    assert(nr_pfns != 0);
    /* Mfns of the batch pfns. */
    ctx->save.primary_mfns = malloc(nr_pfns * sizeof(*ctx->save.primary_mfns));
    //mfns = malloc(nr_pfns * sizeof(*mfns));
    /* Errors from attempting to map the gfns for primary. */
    errors = malloc(nr_pfns * sizeof(*errors));
    /* Errors from attempting to map the gfns for backup. */
    bckp_errors = malloc(nr_pfns * sizeof(*bckp_errors));


    for ( pfn = 0; pfn < nr_pfns; ++pfn )
    {
        ctx->save.primary_mfns[pfn] = ctx->save.ops.pfn_to_gfn(ctx, pfn);
    }

    ctx->save.primary_guest_mapping = xenforeignmemory_map(xch->fmem,
        ctx->domid, PROT_READ, nr_pfns, ctx->save.primary_mfns, errors);

    if ( !ctx->save.primary_guest_mapping )
    {
        PERROR("SR: Failed to map guest pages");
        rc = -1;
        goto err;
    }

    ctx->save.bckp_guest_mapping = xenforeignmemory_map(xch->fmem,
        ctx->save.bckp_domid, PROT_READ | PROT_WRITE, nr_pfns, ctx->save.bckp_mfns, errors);

    if ( !ctx->save.bckp_guest_mapping )
    {
        PERROR("Failed to map backup VMs guest pages");
        rc = -1;
        goto err;
    }

    for ( i = 0; i < nr_pfns; ++i )
    {
        if (errors[i])
        {
                ERROR("Primary VM's Mapping of pfn %#"PRIpfn" (mfn %#"PRIpfn") failed %d",
                      ctx->save.batch_pfns[i], mfns[i], errors[i]);
                rc = -1;
                goto err;
        }

        if (bckp_errors[i])
        {
                ERROR("Backup VM's Mapping of pfn %#"PRIpfn" (mfn %#"PRIpfn") failed %d",
                      ctx->save.batch_pfns[i], ctx->save.bckp_mfns[i], bckp_errors[i]);
                rc = -1;
                goto err;
        }
    }

err:
    free(mfns);
    free(errors);
    free(bckp_errors);

    return rc;
}

static int memcpy_write_batch(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    xen_pfn_t *mfns = NULL;
    xen_pfn_t *types = NULL;

    xen_pfn_t *dirtied_bckp_mfns = NULL;
    /* Index to hold save.batch_pfns. For ease of iterating over  */
    xen_pfn_t *batch_pfns = NULL;

    void *bckp_page;
    void **local_pages = NULL;
    int rc = -1;
    unsigned i, p, nr_pages = 0, nr_pages_mapped = 0;
    unsigned nr_pfns = ctx->save.nr_batch_pfns;
    void *page, *orig_page;

    DPRINTF("Time at sr_bitmap_scan %lld ns", ns_timer());

    assert(nr_pfns != 0);
    /* Types of the batch pfns. */
    types = malloc(nr_pfns * sizeof(*types));
    /* Mfns of the batch pfns. */
    mfns = malloc(nr_pfns * sizeof(*mfns));
    /* Pointers to locally allocated pages.  Need freeing. */
    local_pages = calloc(nr_pfns, sizeof(*local_pages));
   /* Mfns of backup VM to memcpy to */
    dirtied_bckp_mfns = malloc(nr_pfns * sizeof(*dirtied_bckp_mfns));


    if ( !mfns || !types || !local_pages || !dirtied_bckp_mfns)
    {
        ERROR("Unable to allocate arrays for a batch of %u pages",
              nr_pfns);
        goto err;

    }

    batch_pfns = ctx->save.batch_pfns;

    for ( i = 0; i < nr_pfns; ++i )
    {
        //types[i] = mfns[i] = ctx->save.ops.pfn_to_gfn(ctx,
        //                                              ctx->save.batch_pfns[i]);
        types[i] = mfns[i] = ctx->save.primary_mfns[ctx->save.batch_pfns[i]];
        dirtied_bckp_mfns[i] = ctx->save.bckp_mfns[ctx->save.batch_pfns[i]];

        // Likely a ballooned page.
        if ( mfns[i] == INVALID_MFN )
        {
            set_bit(ctx->save.batch_pfns[i], ctx->save.deferred_pages);
            ++ctx->save.nr_deferred_pages;
        }
    }

    rc = xc_get_pfn_type_batch(xch, ctx->domid, nr_pfns, types);
    if ( rc )
    {
        PERROR("Failed to get types for pfn batch");
        goto err;
    }
    rc = -1;

    DPRINTF("Time at sr_mapping %lld ns", ns_timer());

    for ( i = 0; i < nr_pfns; ++i )
    {
        switch ( types[i] )
        {
        case XEN_DOMCTL_PFINFO_BROKEN:
        case XEN_DOMCTL_PFINFO_XALLOC:
        case XEN_DOMCTL_PFINFO_XTAB:
            continue;
        }

        dirtied_bckp_mfns[nr_pages] = dirtied_bckp_mfns[i];
        mfns[nr_pages] = mfns[i];
        ++nr_pages;
    }

    if ( nr_pages > 0 )
    {
        nr_pages_mapped = nr_pages;

        for ( i = 0, p = 0; i < nr_pfns; ++i )
        {
            switch ( types[i] )
            {
            case XEN_DOMCTL_PFINFO_BROKEN:
            case XEN_DOMCTL_PFINFO_XALLOC:
            case XEN_DOMCTL_PFINFO_XTAB:
                continue;
            }

            orig_page = page = ctx->save.primary_guest_mapping + (batch_pfns[p] * PAGE_SIZE);

            rc = ctx->save.ops.normalise_page(ctx, types[i], &page);

            if ( orig_page != page ) /* Only send if it is different */
                local_pages[i] = page;

            if ( rc )
            {
                if ( rc == -1 && errno == EAGAIN )
                {
                    set_bit(ctx->save.batch_pfns[i], ctx->save.deferred_pages);
                    ++ctx->save.nr_deferred_pages;
                    types[i] = XEN_DOMCTL_PFINFO_XTAB;
                    --nr_pages;
                }
                else
                    goto err;
            }

            else
            {
                    bckp_page = ctx->save.bckp_guest_mapping + (batch_pfns[p] * PAGE_SIZE);
                    memcpy(bckp_page, page, PAGE_SIZE);
                    --nr_pages;
            }

            rc = -1;
            ++p;
        }
    }

    /* Sanity check we have sent all the pages we expected to. */
    assert(nr_pages == 0);
    rc = ctx->save.nr_batch_pfns = 0;

    DPRINTF("Time at sr_memcpy %lld ns", ns_timer());

err:
    for ( i = 0; local_pages && i < nr_pfns; ++i )
        free(local_pages[i]);
    free(local_pages);
    free(types);
    free(mfns);

    return rc;
}

static int write_batch(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    xen_pfn_t *mfns = NULL, *types = NULL;
    void *guest_mapping = NULL;
    void **guest_data = NULL;
    void **local_pages = NULL;
    int *errors = NULL, rc = -1;
    unsigned i, p, nr_pages = 0, nr_pages_mapped = 0;
    unsigned nr_pfns = ctx->save.nr_batch_pfns;
    void *page, *orig_page;
    uint64_t *rec_pfns = NULL;
    struct iovec *iov = NULL; int iovcnt = 0;
    struct xc_sr_rec_page_data_header hdr = { 0 };
    struct xc_sr_record rec =
    {
        .type = REC_TYPE_PAGE_DATA,
    };

    assert(nr_pfns != 0);

    /* Mfns of the batch pfns. */
    mfns = malloc(nr_pfns * sizeof(*mfns));
    /* Types of the batch pfns. */
    types = malloc(nr_pfns * sizeof(*types));
    /* Errors from attempting to map the gfns. */
    errors = malloc(nr_pfns * sizeof(*errors));
    /* Pointers to page data to send.  Mapped gfns or local allocations. */
    guest_data = calloc(nr_pfns, sizeof(*guest_data));
    /* Pointers to locally allocated pages.  Need freeing. */
    local_pages = calloc(nr_pfns, sizeof(*local_pages));
    /* iovec[] for writev(). */
    iov = malloc((nr_pfns + 4) * sizeof(*iov));

    if ( !mfns || !types || !errors || !guest_data || !local_pages || !iov )
    {
        ERROR("Unable to allocate arrays for a batch of %u pages",
              nr_pfns);
        goto err;
    }

    for ( i = 0; i < nr_pfns; ++i )
    {
        types[i] = mfns[i] = ctx->save.ops.pfn_to_gfn(ctx,
                                                      ctx->save.batch_pfns[i]);

        /* Likely a ballooned page. */
        if ( mfns[i] == INVALID_MFN )
        {
            set_bit(ctx->save.batch_pfns[i], ctx->save.deferred_pages);
            ++ctx->save.nr_deferred_pages;
        }
    }

    rc = xc_get_pfn_type_batch(xch, ctx->domid, nr_pfns, types);
    if ( rc )
    {
        PERROR("Failed to get types for pfn batch");
        goto err;
    }
    rc = -1;

    for ( i = 0; i < nr_pfns; ++i )
    {
        switch ( types[i] )
        {
        case XEN_DOMCTL_PFINFO_BROKEN:
        case XEN_DOMCTL_PFINFO_XALLOC:
        case XEN_DOMCTL_PFINFO_XTAB:
            continue;
        }

        mfns[nr_pages++] = mfns[i];
    }

    if ( nr_pages > 0 )
    {
        guest_mapping = xenforeignmemory_map(xch->fmem,
            ctx->domid, PROT_READ, nr_pages, mfns, errors);
        if ( !guest_mapping )
        {
            PERROR("Failed to map guest pages");
            goto err;
        }
        nr_pages_mapped = nr_pages;

        for ( i = 0, p = 0; i < nr_pfns; ++i )
        {
            switch ( types[i] )
            {
            case XEN_DOMCTL_PFINFO_BROKEN:
            case XEN_DOMCTL_PFINFO_XALLOC:
            case XEN_DOMCTL_PFINFO_XTAB:
                continue;
            }

            if ( errors[p] )
            {
                ERROR("Mapping of pfn %#"PRIpfn" (mfn %#"PRIpfn") failed %d",
                      ctx->save.batch_pfns[i], mfns[p], errors[p]);
                goto err;
            }

            orig_page = page = guest_mapping + (p * PAGE_SIZE);
            rc = ctx->save.ops.normalise_page(ctx, types[i], &page);

            if ( orig_page != page )
                local_pages[i] = page;

            if ( rc )
            {
                if ( rc == -1 && errno == EAGAIN )
                {
                    set_bit(ctx->save.batch_pfns[i], ctx->save.deferred_pages);
                    ++ctx->save.nr_deferred_pages;
                    types[i] = XEN_DOMCTL_PFINFO_XTAB;
                    --nr_pages;
                }
                else
                    goto err;
            }
            else
                guest_data[i] = page;

            rc = -1;
            ++p;
        }
    }

    rec_pfns = malloc(nr_pfns * sizeof(*rec_pfns));
    if ( !rec_pfns )
    {
        ERROR("Unable to allocate %zu bytes of memory for page data pfn list",
              nr_pfns * sizeof(*rec_pfns));
        goto err;
    }

    hdr.count = nr_pfns;

    rec.length = sizeof(hdr);
    rec.length += nr_pfns * sizeof(*rec_pfns);
    rec.length += nr_pages * PAGE_SIZE;

    for ( i = 0; i < nr_pfns; ++i )
        rec_pfns[i] = ((uint64_t)(types[i]) << 32) | ctx->save.batch_pfns[i];

    iov[0].iov_base = &rec.type;
    iov[0].iov_len = sizeof(rec.type);

    iov[1].iov_base = &rec.length;
    iov[1].iov_len = sizeof(rec.length);

    iov[2].iov_base = &hdr;
    iov[2].iov_len = sizeof(hdr);

    iov[3].iov_base = rec_pfns;
    iov[3].iov_len = nr_pfns * sizeof(*rec_pfns);

    iovcnt = 4;

    if ( nr_pages )
    {
        for ( i = 0; i < nr_pfns; ++i )
        {
            if ( guest_data[i] )
            {
                iov[iovcnt].iov_base = guest_data[i];
                iov[iovcnt].iov_len = PAGE_SIZE;
                iovcnt++;
                --nr_pages;
            }
        }
    }

    if ( writev_exact(ctx->fd, iov, iovcnt) )
    {
        PERROR("Failed to write page data to stream");
        goto err;
    }

    /* Sanity check we have sent all the pages we expected to. */
    assert(nr_pages == 0);
    rc = ctx->save.nr_batch_pfns = 0;

 err:
    free(rec_pfns);
    if ( guest_mapping )
        xenforeignmemory_unmap(xch->fmem, guest_mapping, nr_pages_mapped);
    for ( i = 0; local_pages && i < nr_pfns; ++i )
        free(local_pages[i]);
    free(iov);
    free(local_pages);
    free(guest_data);
    free(errors);
    free(types);
    free(mfns);

    return rc;
}

/*
 * Flush a batch of pfns into the stream.
 */
static int flush_batch(struct xc_sr_context *ctx)
{
    int rc = 0;

    if ( ctx->save.nr_batch_pfns == 0 )
        return rc;

    if( ctx->save.read_mfns )
        rc = memcpy_write_batch(ctx);
    else
        rc = write_batch(ctx);

    if ( !rc )
    {
        VALGRIND_MAKE_MEM_UNDEFINED(ctx->save.batch_pfns,
                                    MAX_BATCH_SIZE *
                                    sizeof(*ctx->save.batch_pfns));
    }

    return rc;
}

/*
 * Add a single pfn to the batch, flushing the batch if full.
 */
static int add_to_batch(struct xc_sr_context *ctx, xen_pfn_t pfn)
{
    int rc = 0;

    if ( ctx->save.nr_batch_pfns == MAX_BATCH_SIZE )
        rc = flush_batch(ctx);

    if ( rc == 0 )
        ctx->save.batch_pfns[ctx->save.nr_batch_pfns++] = pfn;

    return rc;
}

/*
 * Pause/suspend the domain, and refresh ctx->dominfo if required.
 */
static int suspend_domain(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;

    /* TODO: Properly specify the return value from this callback.  All
     * implementations currently appear to return 1 for success, whereas
     * the legacy code checks for != 0. */
    int cb_rc = ctx->save.callbacks->suspend(ctx->save.callbacks->data);

    if ( cb_rc == 0 )
    {
        ERROR("save callback suspend() failed: %d", cb_rc);
        return -1;
    }

    /* Refresh domain information. */
    if ( (xc_domain_getinfo(xch, ctx->domid, 1, &ctx->dominfo) != 1) ||
         (ctx->dominfo.domid != ctx->domid) )
    {
        PERROR("Unable to refresh domain information");
        return -1;
    }

    /* Confirm the domain has actually been paused. */
    if ( !ctx->dominfo.shutdown ||
         (ctx->dominfo.shutdown_reason != SHUTDOWN_suspend) )
    {
        ERROR("Domain has not been suspended: shutdown %d, reason %d",
              ctx->dominfo.shutdown, ctx->dominfo.shutdown_reason);
        return -1;
    }

    xc_report_progress_single(xch, "Domain now suspended");

    return 0;
}

/*
 * Send a subset of pages in the guests p2m, according to the dirty bitmap.
 * Used for each subsequent iteration of the live migration loop.
 *
 * Bitmap is bounded by p2m_size.
 */
static int send_dirty_pages(struct xc_sr_context *ctx,
                            unsigned long entries)
{
    xc_interface *xch = ctx->xch;
    xen_pfn_t p, q, curr_bit;
    unsigned long written;
    int rc;
    xen_pfn_t sz_c = sizeof(int) * 8;

    DECLARE_HYPERCALL_BUFFER_SHADOW(unsigned long, dirty_bitmap,
                                    &ctx->save.dirty_bitmap_hbuf);

    int *bmp = (int *)dirty_bitmap;

    DPRINTF("SR: p2m size is %ld", ctx->save.p2m_size);

    DPRINTF("Time at sr_add2batch_start %lld ns", ns_timer());

    for ( p = 0, written = 0; p < ctx->save.p2m_size/sz_c; ++p )
    {
        if (bmp[p] == 0)
            continue;

        for ( q = 0; q < sz_c; ++q )
        {
            curr_bit = ( p * sz_c ) + q;
            if ( !test_bit(curr_bit, dirty_bitmap) )
                continue;

            rc = add_to_batch(ctx, curr_bit);
            if ( rc )
                return rc;

            /* Update progress every 4MB worth of memory sent. */
            if ( (written & ((1U << (22 - 12)) - 1)) == 0 )
                xc_report_progress_step(xch, written, entries);

            ++written;
        }
    }

    DPRINTF("Time at sr_add2batch_end %lld ns", ns_timer());

    rc = flush_batch(ctx);

    DPRINTF("Time at sr_flushbatch_end %lld ns", ns_timer());

    if ( rc )
        return rc;

    if ( written > entries )
        DPRINTF("Bitmap contained more entries than expected...");

    xc_report_progress_step(xch, entries, entries);

    return ctx->save.ops.check_vm_state(ctx);
}

/*
 * Send all pages in the guests p2m.  Used as the first iteration of the live
 * migration loop, and for a non-live save.
 */
static int send_all_pages(struct xc_sr_context *ctx)
{
    DECLARE_HYPERCALL_BUFFER_SHADOW(unsigned long, dirty_bitmap,
                                    &ctx->save.dirty_bitmap_hbuf);

    bitmap_set(dirty_bitmap, ctx->save.p2m_size);

    return send_dirty_pages(ctx, ctx->save.p2m_size);
}

static int enable_logdirty(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    int on1 = 0, off = 0, on2 = 0;
    int rc;

    /* This juggling is required if logdirty is enabled for VRAM tracking. */
    rc = xc_shadow_control(xch, ctx->domid,
                           XEN_DOMCTL_SHADOW_OP_ENABLE_LOGDIRTY,
                           NULL, 0, NULL, 0, NULL);
    if ( rc < 0 )
    {
        on1 = errno;
        rc = xc_shadow_control(xch, ctx->domid, XEN_DOMCTL_SHADOW_OP_OFF,
                               NULL, 0, NULL, 0, NULL);
        if ( rc < 0 )
            off = errno;
        else {
            rc = xc_shadow_control(xch, ctx->domid,
                                   XEN_DOMCTL_SHADOW_OP_ENABLE_LOGDIRTY,
                                   NULL, 0, NULL, 0, NULL);
            if ( rc < 0 )
                on2 = errno;
        }
        if ( rc < 0 )
        {
            PERROR("Failed to enable logdirty: %d,%d,%d", on1, off, on2);
            return rc;
        }
    }

    return 0;
}

static int update_progress_string(struct xc_sr_context *ctx,
                                  char **str, unsigned iter)
{
    xc_interface *xch = ctx->xch;
    char *new_str = NULL;

    if ( asprintf(&new_str, "Frames iteration %u of %u",
                  iter, ctx->save.max_iterations) == -1 )
    {
        PERROR("Unable to allocate new progress string");
        return -1;
    }

    free(*str);
    *str = new_str;

    xc_set_progress_prefix(xch, *str);
    return 0;
}

/*
 * Send memory while guest is running.
 */
static int send_memory_live(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    xc_shadow_op_stats_t stats = { 0, ctx->save.p2m_size };
    char *progress_str = NULL;
    unsigned x;
    int rc;

    rc = update_progress_string(ctx, &progress_str, 0);
    if ( rc )
        goto out;

    rc = send_all_pages(ctx);
    if ( rc )
        goto out;

    for ( x = 1;
          ((x < ctx->save.max_iterations) &&
           (stats.dirty_count > ctx->save.dirty_threshold)); ++x )
    {
        if ( xc_shadow_control(
                 xch, ctx->domid, XEN_DOMCTL_SHADOW_OP_CLEAN,
                 &ctx->save.dirty_bitmap_hbuf, ctx->save.p2m_size,
                 NULL, 0, &stats) != ctx->save.p2m_size )
        {
            PERROR("Failed to retrieve logdirty bitmap");
            rc = -1;
            goto out;
        }

        if ( stats.dirty_count == 0 )
            break;

        rc = update_progress_string(ctx, &progress_str, x);
        if ( rc )
            goto out;

        rc = send_dirty_pages(ctx, stats.dirty_count);
        if ( rc )
            goto out;
    }

 out:
    xc_set_progress_prefix(xch, NULL);
    free(progress_str);
    return rc;
}

static int colo_merge_secondary_dirty_bitmap(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    struct xc_sr_record rec = { 0, 0, NULL };
    uint64_t *pfns = NULL;
    uint64_t pfn;
    unsigned count, i;
    int rc;
    DECLARE_HYPERCALL_BUFFER_SHADOW(unsigned long, dirty_bitmap,
                                    &ctx->save.dirty_bitmap_hbuf);

    rc = read_record(ctx, ctx->save.recv_fd, &rec);
    if ( rc )
        goto err;

    if ( rec.type != REC_TYPE_CHECKPOINT_DIRTY_PFN_LIST )
    {
        PERROR("Expect dirty bitmap record, but received %u", rec.type );
        rc = -1;
        goto err;
    }

    if ( rec.length % sizeof(*pfns) )
    {
        PERROR("Invalid dirty pfn list record length %u", rec.length );
        rc = -1;
        goto err;
    }

    count = rec.length / sizeof(*pfns);
    pfns = rec.data;

    for ( i = 0; i < count; i++ )
    {
        pfn = pfns[i];
        if (pfn > ctx->save.p2m_size)
        {
            PERROR("Invalid pfn 0x%" PRIx64, pfn);
            rc = -1;
            goto err;
        }

        set_bit(pfn, dirty_bitmap);
    }

    rc = 0;

 err:
    free(rec.data);
    return rc;
}

/*
 * Sunny: Read the file written by restore code.
 */
static int get_mfns_from_backup(struct xc_sr_context *ctx)
{
    FILE *file = fopen("/tmp/test.txt", "r");
    unsigned long num, i = 0;
    int rc = 0;
    int a;
    unsigned nr_pfns = ctx->save.p2m_size;
    ctx->save.bckp_mfns = malloc(nr_pfns * sizeof(*ctx->save.bckp_mfns));
    if ( !ctx->save.bckp_mfns )
    {
        rc = -1;
        goto err;
    }

    a = fscanf(file, "%d", &ctx->save.bckp_domid);
    while(fscanf(file, "%lu", &num) > 0) {
        ctx->save.bckp_mfns[i] = num;
        i++;
    }
    fclose(file);
err:
    return rc;
}

/*
 * Suspend the domain and send dirty memory.
 * This is the last iteration of the live migration and the
 * heart of the checkpointed stream.
 */
static int suspend_and_send_dirty(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    xc_shadow_op_stats_t stats = { 0, ctx->save.p2m_size };
    char *progress_str = NULL;

#ifdef ENABLE_LIBVMI
    char * xen_write_ff = NULL;
    char * xen_read_ff = NULL;
    char* start_addr = "ffff88001d669177";  //subject to change frequently
//    char* end_addr = "ffff88001d66917b";    //subject to change frequently
#endif

    int rc;
    DECLARE_HYPERCALL_BUFFER_SHADOW(unsigned long, dirty_bitmap,
                                    &ctx->save.dirty_bitmap_hbuf);

    DPRINTF("Time at sr_suspend_start %lld ns", ns_timer());

    rc = suspend_domain(ctx);

    if ( rc )
        goto out;

    DPRINTF("Time at sr_suspend_end %lld ns\n", ns_timer());

#ifdef ENABLE_LIBVMI

    vmi_req.st_addr = malloc(sizeof(vmi_req.st_addr));
//    vmi_req.en_addr = malloc(sizeof(vmi_req.en_addr));

/*------------------------------------------------------------------------------------*/
    /*
     *  Convert hexa address into uint64
     */
    DPRINTF("Start Address: %s\n", start_addr);
    *(vmi_req.st_addr) = 37433400;//(uint64_t) strtoul(start_addr, NULL, 16);    /* Have to get the address printed in the malloc code */
    DPRINTF("Starting Address in unsigned long int: %" PRIu64 "\n", *(vmi_req.st_addr));
/*
    DPRINTF("End Address: %s\n", end_addr);
    *(vmi_req.en_addr) = (uint64_t) strtoul(end_addr, NULL, strlen(end_addr));
    DPRINTF("End Address in unsigned long int: %" PRIu64 "\n", *(vmi_req.en_addr));
*/
/*-------------------------------------------------------------------------------------*/
    if (counter == 1)
    {
	xen_write_ff = "/tmp/xen_to_vmi";        //Linux Pipe
	xen_read_ff = "/tmp/vmi_to_xen";
        mkfifo(xen_read_ff, 0666);        //Create Pipe 2
        xen_write_fd = open(xen_write_ff, O_WRONLY);      //Open Pipe 1 for Write
        xen_read_fd = open(xen_read_ff, O_RDONLY);      //open Pipe 2 for Read
    }

    DPRINTF("Time at sr_vmi_write %lld ns", ns_timer());

    rc = write(xen_write_fd, vmi_req.st_addr, sizeof(void *));//Write start address to Pipe 1
    fsync(xen_write_fd);
    fprintf(stderr, "Written 1st address %" PRIu64 " Successfully!!\n", *(vmi_req.st_addr));

    fprintf(stderr, "Reading from LibVMI\n");
    rc = read(xen_read_fd, &buf, sizeof(int)); //Read Accept or Reject as 1 or 0
    fprintf(stderr,"REMUS: Received: %d\n", buf);

    DPRINTF("Time at sr_vmi_read %lld ns", ns_timer());

/*--------------------------------------------------------------------------*/
/*
 *  Have to let the first checkpoint pass, as it doesn't send the vcpu information
 */

    if (!buf && counter == 2)
    {
        fprintf(stderr,"REMUS: FAILING OVER HERE: %d\n", buf);
        close(xen_write_fd);
        close(xen_read_fd);
        unlink(xen_read_ff);
        free (vmi_req.st_addr);
        free (vmi_req.en_addr);
        fprintf(stderr, "REMUS: Suspending domain");

        return 100;
    }

    rc = read(xen_read_fd, &buf, sizeof(int)); //Read Accept or Reject as 1 or 0
    if (!buf && counter == 2)
    {
        fprintf(stderr,"REMUS: FAILING OVER HERE: %d\n", buf);
        close(xen_write_fd);
        close(xen_read_fd);
    	unlink(xen_read_ff);
    	free (vmi_req.st_addr);
    	free (vmi_req.en_addr);
        fprintf(stderr, "REMUS: Suspending domain");

    	return 100;
    }
    counter = 2;
#endif

    if ( xc_shadow_control(
             xch, ctx->domid, XEN_DOMCTL_SHADOW_OP_CLEAN,
             HYPERCALL_BUFFER(dirty_bitmap), ctx->save.p2m_size,
             NULL, XEN_DOMCTL_SHADOW_LOGDIRTY_FINAL, &stats) !=
         ctx->save.p2m_size )
    {
        PERROR("Failed to retrieve logdirty bitmap");
        rc = -1;
        goto out;
    }

    if ( ctx->save.live )
    {
        rc = update_progress_string(ctx, &progress_str,
                                    ctx->save.max_iterations);
        if ( rc )
            goto out;
    }
    else
        xc_set_progress_prefix(xch, "Checkpointed save");

    bitmap_or(dirty_bitmap, ctx->save.deferred_pages, ctx->save.p2m_size);

    if ( !ctx->save.live && ctx->save.checkpointed == XC_MIG_STREAM_COLO )
    {
        rc = colo_merge_secondary_dirty_bitmap(ctx);
        if ( rc )
        {
            PERROR("Failed to get secondary vm's dirty pages");
            goto out;
        }
    }

    DPRINTF("SUNNY: Dirty page count is %u pages", stats.dirty_count);
    DPRINTF("Time at sr_dirtypage_start %lld ns", ns_timer());

    rc = send_dirty_pages(ctx, stats.dirty_count + ctx->save.nr_deferred_pages);

    DPRINTF("Time at sr_dirtypage_end %lld ns", ns_timer());

    if ( rc )
        goto out;

    bitmap_clear(ctx->save.deferred_pages, ctx->save.p2m_size);
    ctx->save.nr_deferred_pages = 0;

 out:
    xc_set_progress_prefix(xch, NULL);
    free(progress_str);
    return rc;
}

static int verify_frames(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    xc_shadow_op_stats_t stats = { 0, ctx->save.p2m_size };
    int rc;
    struct xc_sr_record rec =
    {
        .type = REC_TYPE_VERIFY,
        .length = 0,
    };

    DPRINTF("Enabling verify mode");

    rc = write_record(ctx, &rec);
    if ( rc )
        goto out;

    xc_set_progress_prefix(xch, "Frames verify");
    rc = send_all_pages(ctx);
    if ( rc )
        goto out;

    if ( xc_shadow_control(
             xch, ctx->domid, XEN_DOMCTL_SHADOW_OP_PEEK,
             &ctx->save.dirty_bitmap_hbuf, ctx->save.p2m_size,
             NULL, 0, &stats) != ctx->save.p2m_size )
    {
        PERROR("Failed to retrieve logdirty bitmap");
        rc = -1;
        goto out;
    }

    DPRINTF("  Further stats: faults %u, dirty %u",
            stats.fault_count, stats.dirty_count);

 out:
    return rc;
}

/*
 * Send all domain memory.  This is the heart of the live migration loop.
 */
static int send_domain_memory_live(struct xc_sr_context *ctx)
{
    int rc;

    rc = enable_logdirty(ctx);
    if ( rc )
        goto out;

    rc = send_memory_live(ctx);
    if ( rc )
        goto out;

    rc = suspend_and_send_dirty(ctx);
    if ( rc )
        goto out;

    if ( ctx->save.debug && ctx->save.checkpointed != XC_MIG_STREAM_NONE )
    {
        rc = verify_frames(ctx);
        if ( rc )
            goto out;
    }

  out:
    return rc;
}

/*
 * Checkpointed save.
 */
static int send_domain_memory_checkpointed(struct xc_sr_context *ctx)
{
    return suspend_and_send_dirty(ctx);
}

/*
 * Send all domain memory, pausing the domain first.  Generally used for
 * suspend-to-file.
 */
static int send_domain_memory_nonlive(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    int rc;

    rc = suspend_domain(ctx);
    if ( rc )
        goto err;

    xc_set_progress_prefix(xch, "Frames");

    rc = send_all_pages(ctx);
    if ( rc )
        goto err;

 err:
    return rc;
}

static int setup(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    int rc;
    DECLARE_HYPERCALL_BUFFER_SHADOW(unsigned long, dirty_bitmap,
                                    &ctx->save.dirty_bitmap_hbuf);

    rc = ctx->save.ops.setup(ctx);
    if ( rc )
        goto err;

    dirty_bitmap = xc_hypercall_buffer_alloc_pages(
                   xch, dirty_bitmap, NRPAGES(bitmap_size(ctx->save.p2m_size)));
    ctx->save.batch_pfns = malloc(MAX_BATCH_SIZE *
                                  sizeof(*ctx->save.batch_pfns));
    ctx->save.deferred_pages = calloc(1, bitmap_size(ctx->save.p2m_size));

    if ( !ctx->save.batch_pfns || !dirty_bitmap || !ctx->save.deferred_pages )
    {
        ERROR("Unable to allocate memory for dirty bitmaps, batch pfns and"
              " deferred pages");
        rc = -1;
        errno = ENOMEM;
        goto err;
    }

    rc = 0;

 err:
    return rc;
}

static void cleanup(struct xc_sr_context *ctx)
{
    xc_interface *xch = ctx->xch;
    DECLARE_HYPERCALL_BUFFER_SHADOW(unsigned long, dirty_bitmap,
                                    &ctx->save.dirty_bitmap_hbuf);


    xc_shadow_control(xch, ctx->domid, XEN_DOMCTL_SHADOW_OP_OFF,
                      NULL, 0, NULL, 0, NULL);

    if ( ctx->save.ops.cleanup(ctx) )
        PERROR("Failed to clean up");

    xc_hypercall_buffer_free_pages(xch, dirty_bitmap,
                                   NRPAGES(bitmap_size(ctx->save.p2m_size)));
    free(ctx->save.deferred_pages);
    free(ctx->save.batch_pfns);
}

/*
 * Save a domain.
 */
static int save(struct xc_sr_context *ctx, uint16_t guest_type)
{
    xc_interface *xch = ctx->xch;
    int rc, saved_rc = 0, saved_errno = 0;

    IPRINTF("Saving domain %d, type %s",
            ctx->domid, dhdr_type_to_str(guest_type));

    rc = setup(ctx);
    if ( rc )
        goto err;

    xc_report_progress_single(xch, "Start of stream");

    rc = write_headers(ctx, guest_type);
    if ( rc )
        goto err;

    rc = ctx->save.ops.start_of_stream(ctx);
    if ( rc )
        goto err;

    do {
        rc = ctx->save.ops.start_of_checkpoint(ctx);
        if ( rc )
            goto err;

        rc = ctx->save.ops.check_vm_state(ctx);
        if ( rc )
            goto err;

        DPRINTF("SUNNY: starting migration, suspending domain");

        if ( ctx->save.live ) {
            rc = send_domain_memory_live(ctx);
            DPRINTF("SUNNY: Finished sending live memory");
        }
        else if ( ctx->save.checkpointed != XC_MIG_STREAM_NONE ) {
            DPRINTF("SUNNY: starting checkpointing mechanism");
            rc = send_domain_memory_checkpointed(ctx);
        }
        else
            rc = send_domain_memory_nonlive(ctx);

        if (rc == 100)
        {
            rc = system ("sudo xl pause opensuse64");    //pause the primary
            return 100;
        }

        if ( !ctx->dominfo.shutdown ||
             (ctx->dominfo.shutdown_reason != SHUTDOWN_suspend) )
        {
            ERROR("Domain has not been suspended");
            rc = -1;
            goto err;
        }

       rc = ctx->save.ops.end_of_checkpoint(ctx);
       DPRINTF("SR: Number of end checkpoints sent: %u", ++nr_end_checkpoint);

        if ( rc )
            goto err;

        if ( ctx->save.checkpointed != XC_MIG_STREAM_NONE )
        {
           /*
             * We have now completed the initial live portion of the checkpoint
             * process. Therefore switch into periodically sending synchronous
             * batches of pages.
             */
            ctx->save.live = false;

            rc = write_checkpoint_record(ctx);
            if ( rc )
                goto err;

            if ( ctx->save.checkpointed == XC_MIG_STREAM_COLO )
            {
                rc = ctx->save.callbacks->checkpoint(ctx->save.callbacks->data);
                if ( !rc )
                {
                    rc = -1;
                    goto err;
                }
            }

            rc = ctx->save.callbacks->postcopy(ctx->save.callbacks->data);

            DPRINTF("Time at sr_resume %lld ns", ns_timer());

            if ( rc <= 0 )
                goto err;

            if ( ctx->save.checkpointed == XC_MIG_STREAM_COLO )
            {
                rc = ctx->save.callbacks->wait_checkpoint(
                    ctx->save.callbacks->data);
                if ( rc <= 0 )
                    goto err;
            }
            else if ( ctx->save.checkpointed == XC_MIG_STREAM_REMUS )
            {
                rc = ctx->save.callbacks->checkpoint(ctx->save.callbacks->data);
                if ( rc <= 0 )
                    goto err;
            }
            else
            {
                ERROR("Unknown checkpointed stream");
                rc = -1;
                goto err;
            }
        }
        /*
         *  For not sending pages through writev,
         *  we copy the backup's pages into a file
         *  and read those memory pages into the primary
         */
        //if ( ctx->save.read_mfns == 123 )
        if ( !ctx->save.read_mfns )
        {
            if( get_mfns_from_backup(ctx) )
                DPRINTF("SR: Didn't read mfns");
            if( map_primary_and_backup(ctx) )
                DPRINTF("SR: error: Mapping primary and backup failed");

            ctx->save.read_mfns = 1;
        }

   } while ( ctx->save.checkpointed != XC_MIG_STREAM_NONE );

   xc_report_progress_single(xch, "End of stream");
   rc = write_end_record(ctx);
   if ( rc )
        goto err;
    rc = 100;
    xc_report_progress_single(xch, "Complete");
   goto done;

 err:
    saved_errno = errno;
    saved_rc = rc;
    PERROR("Save failed");

 done:
    cleanup(ctx);

    free(ctx->save.bckp_mfns);

    if ( saved_rc )
    {
        rc = saved_rc;
        errno = saved_errno;
    }

    return rc;

};

int xc_domain_save(xc_interface *xch, int io_fd, uint32_t dom,
                   uint32_t max_iters, uint32_t max_factor, uint32_t flags,
                   struct save_callbacks* callbacks, int hvm,
                   xc_migration_stream_t stream_type, int recv_fd)
{
    struct xc_sr_context ctx =
        {
            .xch = xch,
            .fd = io_fd,
        };

    /* GCC 4.4 (of CentOS 6.x vintage) can' t initialise anonymous unions. */
    ctx.save.callbacks = callbacks;
    ctx.save.live  = !!(flags & XCFLAGS_LIVE);
    ctx.save.debug = !!(flags & XCFLAGS_DEBUG);
    ctx.save.checkpointed = stream_type;
    ctx.save.recv_fd = recv_fd;

    /* If altering migration_stream update this assert too. */
    assert(stream_type == XC_MIG_STREAM_NONE ||
           stream_type == XC_MIG_STREAM_REMUS ||
           stream_type == XC_MIG_STREAM_COLO);

    /*
     * TODO: Find some time to better tweak the live migration algorithm.
     *
     * These parameters are better than the legacy algorithm especially for
     * busy guests.
     */
    ctx.save.max_iterations = 5;
    ctx.save.dirty_threshold = 50;
    ctx.save.read_mfns = 0;

    /* Sanity checks for callbacks. */
    if ( hvm )
        assert(callbacks->switch_qemu_logdirty);
    if ( ctx.save.checkpointed )
        assert(callbacks->checkpoint && callbacks->postcopy);
    if ( ctx.save.checkpointed == XC_MIG_STREAM_COLO )
        assert(callbacks->wait_checkpoint);

    DPRINTF("fd %d, dom %u, max_iters %u, max_factor %u, flags %u, hvm %d",
            io_fd, dom, max_iters, max_factor, flags, hvm);

    if ( xc_domain_getinfo(xch, dom, 1, &ctx.dominfo) != 1 )
    {
        PERROR("Failed to get domain info");
        return -1;
    }

    if ( ctx.dominfo.domid != dom )
    {
        ERROR("Domain %u does not exist", dom);
        return -1;
    }

    ctx.domid = dom;

    if ( ctx.dominfo.hvm )
    {
        ctx.save.ops = save_ops_x86_hvm;
        return save(&ctx, DHDR_TYPE_X86_HVM);
    }
    else
    {
        ctx.save.ops = save_ops_x86_pv;
        return save(&ctx, DHDR_TYPE_X86_PV);
    }
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
