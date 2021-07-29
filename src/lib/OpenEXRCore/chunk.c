/*
** SPDX-License-Identifier: BSD-3-Clause
** Copyright Contributors to the OpenEXR Project.
*/

#include "openexr_chunkio.h"

#include "internal_coding.h"
#include "internal_structs.h"
#include "internal_xdr.h"

#include <limits.h>
#include <string.h>

/**************************************/

/* for testing, we include a bunch of internal stuff into the unit tests which are in c++ */
#if defined __has_include
#    if __has_include(<stdatomic.h>)
#        define EXR_HAS_STD_ATOMICS 1
#    endif
#endif

#ifdef EXR_HAS_STD_ATOMICS
#    include <stdatomic.h>
#elif defined(_MSC_VER)

/* msvc w/ c11 support is only very new, until we know what the preprocessor checks are, provide defaults */
#    include <windows.h>

#    define atomic_load(object) InterlockedOr64 ((int64_t volatile*) object, 0)

static inline int
atomic_compare_exchange_strong (
    uint64_t volatile* object, uint64_t* expected, uint64_t desired)
{
    uint64_t prev =
        (uint64_t) InterlockedCompareExchange64 (object, desired, *expected);
    if (prev == *expected) return 1;
    *expected = prev;
    return 0;
}

#else
#    error OS unimplemented support for atomics
#endif

/**************************************/

static exr_result_t
extract_chunk_table (
    const struct _internal_exr_context* ctxt,
    const struct _internal_exr_part*    part,
    uint64_t**                          chunktable)
{
    uint64_t* ctable = NULL;

    ctable = (uint64_t*) atomic_load (
        EXR_CONST_CAST (atomic_uintptr_t*, &(part->chunk_table)));
    if (ctable == NULL)
    {
        uint64_t  chunkoff   = part->chunk_table_offset;
        uint64_t  chunkbytes = sizeof (uint64_t) * (uint64_t) part->chunk_count;
        int64_t   nread      = 0;
        uintptr_t eptr = 0, nptr = 0;

        exr_result_t rv;

        if (part->chunk_count <= 0)
            return ctxt->report_error (
                ctxt, EXR_ERR_INVALID_ARGUMENT, "Invalid file with no chunks");

        ctable = (uint64_t*) ctxt->alloc_fn (chunkbytes);
        if (ctable == NULL)
            return ctxt->standard_error (ctxt, EXR_ERR_OUT_OF_MEMORY);

        rv = ctxt->do_read (
            ctxt, ctable, chunkbytes, &chunkoff, &nread, EXR_MUST_READ_ALL);
        if (rv != EXR_ERR_SUCCESS)
        {
            ctxt->free_fn (ctable);
            return rv;
        }
        priv_to_native64 (ctable, part->chunk_count);

        //EXR_GETFILE(f)->report_error( ctxt, EXR_ERR_UNKNOWN, "TODO: implement reconstructLineOffsets and similar" );
        nptr = (uintptr_t) ctable;
        // see if we win or not
        if (!atomic_compare_exchange_strong (
                EXR_CONST_CAST (atomic_uintptr_t*, &(part->chunk_table)),
                &eptr,
                nptr))
        {
            ctxt->free_fn (ctable);
            ctable = (uint64_t*) eptr;
            if (ctable == NULL)
                return ctxt->standard_error (ctxt, EXR_ERR_OUT_OF_MEMORY);
        }
    }

    *chunktable = ctable;
    return EXR_ERR_SUCCESS;
}

/**************************************/

static exr_result_t
alloc_chunk_table (
    const struct _internal_exr_context* ctxt,
    const struct _internal_exr_part*    part,
    uint64_t**                          chunktable)
{
    uint64_t* ctable = NULL;

    /* we have the lock, but to access the type, we'll use the atomic function anyway */
    ctable = (uint64_t*) atomic_load (
        EXR_CONST_CAST (atomic_uintptr_t*, &(part->chunk_table)));
    if (ctable == NULL)
    {
        uint64_t  chunkbytes = sizeof (uint64_t) * (uint64_t) part->chunk_count;
        uintptr_t eptr = 0, nptr = 0;

        ctable = (uint64_t*) ctxt->alloc_fn (chunkbytes);
        if (ctable == NULL)
            return ctxt->standard_error (ctxt, EXR_ERR_OUT_OF_MEMORY);
        memset (ctable, 0, chunkbytes);

        nptr = (uintptr_t) ctable;
        if (!atomic_compare_exchange_strong (
                EXR_CONST_CAST (atomic_uintptr_t*, &(part->chunk_table)),
                &eptr,
                nptr))
        {
            ctxt->free_fn (ctable);
            ctable = (uint64_t*) eptr;
            if (ctable == NULL)
                return ctxt->standard_error (ctxt, EXR_ERR_OUT_OF_MEMORY);
        }
    }
    *chunktable = ctable;
    return EXR_ERR_SUCCESS;
}

/**************************************/

exr_result_t
exr_read_scanline_chunk_info (
    exr_const_context_t ctxt, int part_index, int y, exr_chunk_info_t* cinfo)
{
    exr_result_t     rv;
    int              miny, cidx, rdcnt, lpc;
    int32_t          data[3];
    int64_t          ddata[3];
    int64_t          fsize;
    uint64_t         dataoff;
    exr_attr_box2i_t dw;
    uint64_t*        ctable;
    EXR_PROMOTE_READ_CONST_CONTEXT_AND_PART_OR_ERROR (ctxt, part_index);

    if (!cinfo) return pctxt->standard_error (pctxt, EXR_ERR_INVALID_ARGUMENT);

    if (part->storage_mode == EXR_STORAGE_TILED ||
        part->storage_mode == EXR_STORAGE_DEEP_TILED)
    {
        return pctxt->standard_error (pctxt, EXR_ERR_SCAN_TILE_MIXEDAPI);
    }

    dw = part->data_window;
    if (y < dw.min.y || y > dw.max.y)
    {
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "Invalid request for scanline %d outside range of data window (%d - %d)",
            y,
            dw.min.y,
            dw.max.y);
    }

    lpc  = part->lines_per_chunk;
    cidx = (y - dw.min.y);
    if (lpc > 1) cidx /= lpc;

    // do we need to invert this when reading decreasing y? it appears not
    //if (part->lineorder == EXR_LINEORDER_DECREASING_Y)
    //    cidx = part->chunk_count - (cidx + 1);
    miny = (dw.min.y + cidx * lpc);

    if (cidx < 0 || cidx >= part->chunk_count)
    {
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "Invalid request for scanline %d in chunk %d outside chunk count %d",
            y,
            cidx,
            part->chunk_count);
    }

    cinfo->idx         = cidx;
    cinfo->type        = (uint8_t) part->storage_mode;
    cinfo->compression = (uint8_t) part->comp_type;
    cinfo->start_x     = dw.min.x;
    cinfo->start_y     = miny;
    cinfo->width       = dw.max.x - dw.min.x + 1;
    cinfo->height      = lpc;
    if (miny < dw.min.y)
    {
        cinfo->start_y = dw.min.y;
        cinfo->height -= (dw.min.y - miny);
    }
    else if ((miny + lpc) > dw.max.y)
    {
        cinfo->height = (dw.max.y - miny + 1);
    }
    cinfo->level_x = 0;
    cinfo->level_y = 0;

    /* need to read from the file to get the packed chunk size */
    rv = extract_chunk_table (pctxt, part, &ctable);
    if (rv != EXR_ERR_SUCCESS) return rv;

    dataoff = ctable[cidx];
    /* multi part files have the part for validation */
    rdcnt = (pctxt->is_multipart) ? 2 : 1;
    /* deep has 64-bit data, so be variable about what we read */
    if (part->storage_mode != EXR_STORAGE_DEEP_SCANLINE) ++rdcnt;

    rv = pctxt->do_read (
        pctxt,
        data,
        (size_t) (rdcnt) * sizeof (int32_t),
        &dataoff,
        NULL,
        EXR_MUST_READ_ALL);

    if (rv != EXR_ERR_SUCCESS) return rv;

    priv_to_native32 (data, rdcnt);

    rdcnt = 0;
    if (pctxt->is_multipart)
    {
        if (data[rdcnt] != part_index)
        {
            return pctxt->print_error (
                pctxt,
                EXR_ERR_BAD_CHUNK_LEADER,
                "Preparing read scanline %d (chunk %d), found corrupt leader: part says %d, expected %d",
                y,
                cidx,
                data[rdcnt],
                part_index);
        }
        ++rdcnt;
    }
    if (miny != data[rdcnt])
    {
        return pctxt->print_error (
            pctxt,
            EXR_ERR_BAD_CHUNK_LEADER,
            "Preparing to read scanline %d (chunk %d), found corrupt leader: scanline says %d, expected %d",
            y,
            cidx,
            data[rdcnt],
            miny);
    }

    fsize = pctxt->file_size;
    if (part->storage_mode == EXR_STORAGE_DEEP_SCANLINE)
    {
        rv = pctxt->do_read (
            pctxt,
            ddata,
            3 * sizeof (int64_t),
            &dataoff,
            NULL,
            EXR_MUST_READ_ALL);
        if (rv != EXR_ERR_SUCCESS) { return rv; }
        priv_to_native64 (ddata, 3);

        if (ddata[0] < 0)
        {
            return pctxt->print_error (
                pctxt,
                EXR_ERR_BAD_CHUNK_LEADER,
                "Preparing to read scanline %d (chunk %d), found corrupt leader: invalid sample table size %" PRId64,
                y,
                cidx,
                ddata[0]);
        }
        if (ddata[1] < 0 || ddata[1] > (int64_t) INT_MAX)
        {
            return pctxt->print_error (
                pctxt,
                EXR_ERR_BAD_CHUNK_LEADER,
                "Preparing to read scanline %d (chunk %d), found corrupt leader: invalid packed data size %" PRId64,
                y,
                cidx,
                ddata[1]);
        }
        if (ddata[2] < 0 || ddata[2] > (int64_t) INT_MAX)
        {
            return pctxt->print_error (
                pctxt,
                EXR_ERR_BAD_CHUNK_LEADER,
                "Preparing to scanline %d (chunk %d), found corrupt leader: unsupported unpacked data size %" PRId64,
                y,
                cidx,
                ddata[2]);
        }

        cinfo->sample_count_data_offset = dataoff;
        cinfo->sample_count_table_size  = (uint64_t) ddata[0];
        cinfo->data_offset              = dataoff + (uint64_t) ddata[0];
        cinfo->packed_size              = (uint64_t) ddata[1];
        cinfo->unpacked_size            = (uint64_t) ddata[2];

        if (fsize > 0 &&
            ((cinfo->sample_count_data_offset +
              cinfo->sample_count_table_size) > ((uint64_t) fsize) ||
             (cinfo->data_offset + cinfo->packed_size) > ((uint64_t) fsize)))
        {
            return pctxt->print_error (
                pctxt,
                EXR_ERR_BAD_CHUNK_LEADER,
                "Preparing to scanline %d (chunk %d), found corrupt leader: sample table and data result in access past end of the file: sample table size %" PRId64
                " + data size %" PRId64 " larger than file %" PRId64,
                y,
                cidx,
                ddata[0],
                ddata[1],
                fsize);
        }
    }
    else
    {
        uint64_t unpacksize = 0;
        if (cinfo->height == lpc)
        {
            unpacksize = part->unpacked_size_per_chunk;
        }
        else
        {
            const exr_attr_chlist_t* chanlist = part->channels->chlist;
            for (int c = 0; c < chanlist->num_channels; ++c)
            {
                const exr_attr_chlist_entry_t* curc = (chanlist->entries + c);
                if (curc->y_sampling > 1 || curc->x_sampling > 1)
                    unpacksize +=
                        (((uint64_t) (cinfo->height / curc->y_sampling)) *
                         ((uint64_t) (cinfo->width / curc->x_sampling)) *
                         ((curc->pixel_type == EXR_PIXEL_HALF) ? 2 : 4));
                else
                    unpacksize +=
                        ((uint64_t) cinfo->height) * ((uint64_t) cinfo->width) *
                        ((curc->pixel_type == EXR_PIXEL_HALF) ? 2 : 4);
            }
        }

        ++rdcnt;
        if (data[rdcnt] < 0 ||
            (uint64_t) data[rdcnt] > part->unpacked_size_per_chunk)
        {
            return pctxt->print_error (
                pctxt,
                EXR_ERR_BAD_CHUNK_LEADER,
                "Preparing to read scanline %d (chunk %d), found corrupt leader: packed data size says %" PRIu64
                ", must be between 0 and %" PRIu64,
                y,
                cidx,
                (uint64_t) data[rdcnt],
                part->unpacked_size_per_chunk);
        }

        cinfo->data_offset              = dataoff;
        cinfo->packed_size              = (uint64_t) data[rdcnt];
        cinfo->unpacked_size            = unpacksize;
        cinfo->sample_count_data_offset = 0;
        cinfo->sample_count_table_size  = 0;

        if (fsize > 0 &&
            (cinfo->data_offset + cinfo->packed_size) > ((uint64_t) fsize))
        {
            return pctxt->print_error (
                pctxt,
                EXR_ERR_BAD_CHUNK_LEADER,
                "Preparing to read scanline %d (chunk %d), found corrupt leader: packed size %" PRIu64
                ", file size %" PRId64,
                y,
                cidx,
                (uint64_t) data[rdcnt],
                fsize);
        }
    }
    return EXR_ERR_SUCCESS;
}

/**************************************/

static exr_result_t
compute_tile_chunk_off (
    const struct _internal_exr_context* ctxt,
    const struct _internal_exr_part*    part,
    int                                 tilex,
    int                                 tiley,
    int                                 levelx,
    int                                 levely,
    int32_t*                            chunkoffout)
{
    int                        numx, numy;
    int64_t                    chunkoff = 0;
    const exr_attr_tiledesc_t* tiledesc = part->tiles->tiledesc;

    switch (EXR_GET_TILE_LEVEL_MODE ((*tiledesc)))
    {
        case EXR_TILE_ONE_LEVEL:
        case EXR_TILE_MIPMAP_LEVELS:
            if (levelx != levely)
            {
                return ctxt->print_error (
                    ctxt,
                    EXR_ERR_INVALID_ARGUMENT,
                    "Request for tile (%d, %d) level (%d, %d), but single level and mipmap tiles must have same level x and y",
                    tilex,
                    tiley,
                    levelx,
                    levely);
            }
            if (levelx >= part->num_tile_levels_x)
            {
                return ctxt->print_error (
                    ctxt,
                    EXR_ERR_INVALID_ARGUMENT,
                    "Request for tile (%d, %d) level %d, but level past available levels (%d)",
                    tilex,
                    tiley,
                    levelx,
                    part->num_tile_levels_x);
            }

            numx = part->tile_level_tile_count_x[levelx];
            numy = part->tile_level_tile_count_y[levelx];

            if (tilex >= numx || tiley >= numy)
            {
                return ctxt->print_error (
                    ctxt,
                    EXR_ERR_INVALID_ARGUMENT,
                    "Request for tile (%d, %d) level %d, but level only has %d x %d tiles",
                    tilex,
                    tiley,
                    levelx,
                    numx,
                    numy);
            }

            for (int l = 0; l < levelx; ++l)
                chunkoff +=
                    ((int64_t) part->tile_level_tile_count_x[l] *
                     (int64_t) part->tile_level_tile_count_y[l]);
            chunkoff += tiley * numx + tilex;
            break;

        case EXR_TILE_RIPMAP_LEVELS:
            if (levelx >= part->num_tile_levels_x)
            {
                return ctxt->print_error (
                    ctxt,
                    EXR_ERR_INVALID_ARGUMENT,
                    "Request for tile (%d, %d) level %d, %d, but x level past available levels (%d)",
                    tilex,
                    tiley,
                    levelx,
                    levely,
                    part->num_tile_levels_x);
            }
            if (levely >= part->num_tile_levels_y)
            {
                return ctxt->print_error (
                    ctxt,
                    EXR_ERR_INVALID_ARGUMENT,
                    "Request for tile (%d, %d) level %d, %d, but y level past available levels (%d)",
                    tilex,
                    tiley,
                    levelx,
                    levely,
                    part->num_tile_levels_y);
            }

            numx = part->tile_level_tile_count_x[levelx];
            numy = part->tile_level_tile_count_y[levely];

            if (tilex >= numx || tiley >= numy)
            {
                return ctxt->print_error (
                    ctxt,
                    EXR_ERR_INVALID_ARGUMENT,
                    "Request for tile (%d, %d) at rip level %d, %d level only has %d x %d tiles",
                    tilex,
                    tiley,
                    levelx,
                    levely,
                    numx,
                    numy);
            }

            for (int ly = 0; ly < levely; ++ly)
            {
                for (int lx = 0; lx < levelx; ++lx)
                {
                    chunkoff +=
                        ((int64_t) part->tile_level_tile_count_x[lx] *
                         (int64_t) part->tile_level_tile_count_y[ly]);
                }
            }
            for (int lx = 0; lx < levelx; ++lx)
            {
                chunkoff +=
                    ((int64_t) part->tile_level_tile_count_x[lx] *
                     (int64_t) numy);
            }
            chunkoff += tiley * numx + tilex;
            break;
        case EXR_TILE_LAST_TYPE:
        default:
            return ctxt->print_error (
                ctxt, EXR_ERR_UNKNOWN, "Invalid tile description");
    }

    if (chunkoff >= part->chunk_count)
    {
        return ctxt->print_error (
            ctxt,
            EXR_ERR_UNKNOWN,
            "Invalid tile chunk offset %" PRId64 " (%d avail)",
            chunkoff,
            part->chunk_count);
    }

    *chunkoffout = (int32_t) chunkoff;
    return EXR_ERR_SUCCESS;
}

/**************************************/

exr_result_t
exr_read_tile_chunk_info (
    exr_const_context_t ctxt,
    int                 part_index,
    int                 tilex,
    int                 tiley,
    int                 levelx,
    int                 levely,
    exr_chunk_info_t*   cinfo)
{
    exr_result_t               rv;
    int32_t                    data[6];
    int32_t*                   tdata;
    int32_t                    cidx, ntoread;
    uint64_t                   dataoff;
    int64_t                    fsize, tend, dend;
    const exr_attr_chlist_t*   chanlist;
    const exr_attr_tiledesc_t* tiledesc;
    int                        tilew, tileh, unpacksize = 0;
    uint64_t*                  ctable;
    EXR_PROMOTE_READ_CONST_CONTEXT_AND_PART_OR_ERROR (ctxt, part_index);

    if (!cinfo) return pctxt->standard_error (pctxt, EXR_ERR_INVALID_ARGUMENT);

    if (tilex < 0 || tiley < 0 || levelx < 0 || levely < 0)
        return pctxt->standard_error (pctxt, EXR_ERR_INVALID_ARGUMENT);
    if (part->storage_mode == EXR_STORAGE_SCANLINE ||
        part->storage_mode == EXR_STORAGE_DEEP_SCANLINE)
    {
        return pctxt->standard_error (pctxt, EXR_ERR_TILE_SCAN_MIXEDAPI);
    }

    if (!part->tiles || part->num_tile_levels_x <= 0 ||
        part->num_tile_levels_y <= 0 || !part->tile_level_tile_count_x ||
        !part->tile_level_tile_count_y)
    {
        return pctxt->print_error (
            pctxt, EXR_ERR_MISSING_REQ_ATTR, "Tile data missing or corrupt");
    }

    tiledesc = part->tiles->tiledesc;

    tilew = (int) (tiledesc->x_size);
    dend  = ((int64_t) part->tile_level_tile_size_x[levelx]);
    tend  = ((int64_t) tilew) * ((int64_t) (tilex + 1));
    if (tend > dend)
    {
        tend -= dend;
        if (tend < tilew) tilew = tilew - ((int) tend);
    }

    tileh = (int) (tiledesc->y_size);
    dend  = ((int64_t) part->tile_level_tile_size_y[levely]);
    tend  = ((int64_t) tileh) * ((int64_t) (tiley + 1));
    if (tend > dend)
    {
        tend -= dend;
        if (tend < tileh) tileh = tileh - ((int) tend);
    }

    cidx = 0;
    rv   = compute_tile_chunk_off (
        pctxt, part, tilex, tiley, levelx, levely, &cidx);
    if (rv != EXR_ERR_SUCCESS) return rv;

    cinfo->idx         = cidx;
    cinfo->type        = (uint8_t) part->storage_mode;
    cinfo->compression = (uint8_t) part->comp_type;
    cinfo->start_x     = tilex;
    cinfo->start_y     = tiley;
    cinfo->height      = tileh;
    cinfo->width       = tilew;
    if (levelx > 255 || levely > 255)
        return pctxt->print_error (
            pctxt,
            EXR_ERR_ATTR_SIZE_MISMATCH,
            "Unable to represent tile level %d, %d in chunk structure",
            levelx,
            levely);

    cinfo->level_x = (uint8_t) levelx;
    cinfo->level_y = (uint8_t) levely;

    chanlist = part->channels->chlist;
    for (int c = 0; c < chanlist->num_channels; ++c)
    {
        const exr_attr_chlist_entry_t* curc = (chanlist->entries + c);
        unpacksize +=
            tilew * tileh * ((curc->pixel_type == EXR_PIXEL_HALF) ? 2 : 4);
    }

    rv = extract_chunk_table (pctxt, part, &ctable);
    if (rv != EXR_ERR_SUCCESS) return rv;

    if (part->storage_mode == EXR_STORAGE_DEEP_TILED)
    {
        if (pctxt->is_multipart)
            ntoread = 5;
        else
            ntoread = 4;
    }
    else if (pctxt->is_multipart)
        ntoread = 6;
    else
        ntoread = 5;

    dataoff = ctable[cidx];
    rv      = pctxt->do_read (
        pctxt,
        data,
        (uint64_t) (ntoread) * sizeof (int32_t),
        &dataoff,
        &fsize,
        EXR_MUST_READ_ALL);
    if (rv != EXR_ERR_SUCCESS)
    {
        return pctxt->print_error (
            pctxt,
            rv,
            "Request for tile (%d, %d), level (%d, %d) but unable to read %" PRId64
            " bytes from offset %" PRId64 ", got %" PRId64 " bytes",
            tilex,
            tiley,
            levelx,
            levely,
            (uint64_t) (ntoread) * sizeof (int32_t),
            ctable[cidx],
            fsize);
    }
    priv_to_native32 (data, ntoread);

    tdata = data;
    if (pctxt->is_multipart)
    {
        if (part_index != data[0])
        {
            return pctxt->print_error (
                pctxt,
                EXR_ERR_BAD_CHUNK_LEADER,
                "Preparing to read tile (%d, %d), level (%d, %d) (chunk %d), found corrupt leader: part says %d, expected %d",
                tilex,
                tiley,
                levelx,
                levely,
                cidx,
                data[0],
                part_index);
        }
        ++tdata;
    }
    if (tdata[0] != tilex)
    {
        return pctxt->print_error (
            pctxt,
            EXR_ERR_BAD_CHUNK_LEADER,
            "Preparing to read tile (%d, %d), level (%d, %d) (chunk %d), found corrupt leader: found tile x %d, expect %d",
            tilex,
            tiley,
            levelx,
            levely,
            cidx,
            tdata[0],
            tilex);
    }
    if (tdata[1] != tiley)
    {
        return pctxt->print_error (
            pctxt,
            EXR_ERR_BAD_CHUNK_LEADER,
            "Preparing to read tile (%d, %d), level (%d, %d) (chunk %d), found corrupt leader: found tile y %d, expect %d",
            tilex,
            tiley,
            levelx,
            levely,
            cidx,
            tdata[1],
            tiley);
    }
    if (tdata[2] != levelx)
    {
        return pctxt->print_error (
            pctxt,
            EXR_ERR_BAD_CHUNK_LEADER,
            "Preparing to read tile (%d, %d), level (%d, %d) (chunk %d), found corrupt leader: found tile level x %d, expect %d",
            tilex,
            tiley,
            levelx,
            levely,
            cidx,
            tdata[2],
            levelx);
    }
    if (tdata[3] != levely)
    {
        return pctxt->print_error (
            pctxt,
            EXR_ERR_BAD_CHUNK_LEADER,
            "Preparing to read tile (%d, %d), level (%d, %d) (chunk %d), found corrupt leader: found tile level y %d, expect %d",
            tilex,
            tiley,
            levelx,
            levely,
            cidx,
            tdata[3],
            levely);
    }

    fsize = pctxt->file_size;
    if (part->storage_mode == EXR_STORAGE_DEEP_TILED)
    {
        int64_t ddata[3];
        rv = pctxt->do_read (
            pctxt,
            ddata,
            3 * sizeof (int64_t),
            &dataoff,
            NULL,
            EXR_MUST_READ_ALL);
        if (rv != EXR_ERR_SUCCESS) { return rv; }
        priv_to_native64 (ddata, 3);

        if (ddata[0] < 0)
        {
            return pctxt->print_error (
                pctxt,
                EXR_ERR_BAD_CHUNK_LEADER,
                "Preparing to read deep tile (%d, %d), level (%d, %d) (chunk %d), found corrupt leader: invalid sample table size %" PRId64,
                tilex,
                tiley,
                levelx,
                levely,
                cidx,
                ddata[0]);
        }

        /* not all compressors support 64-bit */
        if (ddata[1] < 0 || ddata[1] > (int64_t) INT32_MAX)
        {
            return pctxt->print_error (
                pctxt,
                EXR_ERR_BAD_CHUNK_LEADER,
                "Preparing to read deep tile (%d, %d), level (%d, %d) (chunk %d), found corrupt leader: invalid packed data size %" PRId64,
                tilex,
                tiley,
                levelx,
                levely,
                cidx,
                ddata[1]);
        }
        if (ddata[2] < 0 || ddata[2] > (int64_t) INT32_MAX)
        {
            return pctxt->print_error (
                pctxt,
                EXR_ERR_BAD_CHUNK_LEADER,
                "Preparing to read deep tile (%d, %d), level (%d, %d) (chunk %d), found corrupt leader: invalid packed data size %" PRId64,
                tilex,
                tiley,
                levelx,
                levely,
                cidx,
                ddata[1]);
        }
        cinfo->sample_count_data_offset = dataoff;
        cinfo->sample_count_table_size  = (uint64_t) ddata[0];
        cinfo->packed_size              = (uint64_t) ddata[1];
        cinfo->unpacked_size            = (uint64_t) ddata[2];
        cinfo->data_offset              = dataoff + (uint64_t) ddata[0];

        if (fsize > 0 &&
            ((cinfo->sample_count_data_offset +
              cinfo->sample_count_table_size) > ((uint64_t) fsize) ||
             (cinfo->data_offset + cinfo->packed_size) > ((uint64_t) fsize)))
        {
            return pctxt->print_error (
                pctxt,
                EXR_ERR_BAD_CHUNK_LEADER,
                "Preparing to read deep tile (%d, %d), level (%d, %d) (chunk %d), found corrupt leader: sample table and data result in access past end of the file: sample table size %" PRId64
                " + data size %" PRId64 " larger than file %" PRId64,
                tilex,
                tiley,
                levelx,
                levely,
                cidx,
                ddata[0],
                ddata[1],
                fsize);
        }
    }
    else
    {
        if (tdata[4] < 0 || tdata[4] > unpacksize ||
            (fsize > 0 && tdata[4] > fsize))
        {
            return pctxt->print_error (
                pctxt,
                EXR_ERR_BAD_CHUNK_LEADER,
                "Preparing to read deep tile (%d, %d), level (%d, %d) (chunk %d), found corrupt leader: invalid packed size (%d) vs unpacked size (%d), and file size %" PRId64,
                tilex,
                tiley,
                levelx,
                levely,
                cidx,
                (int)tdata[4],
                (int)unpacksize,
                fsize);
        }
        cinfo->packed_size              = (uint64_t) tdata[4];
        cinfo->unpacked_size            = (uint64_t) unpacksize;
        cinfo->data_offset              = (uint64_t) dataoff;
        cinfo->sample_count_data_offset = 0;
        cinfo->sample_count_table_size  = 0;
    }
    return EXR_ERR_SUCCESS;
}

exr_result_t
exr_read_chunk (
    exr_const_context_t     ctxt,
    int                     part_index,
    const exr_chunk_info_t* cinfo,
    void*                   packed_data)
{
    exr_result_t                 rv;
    uint64_t                     dataoffset, toread;
    int64_t                      nread;
    enum _INTERNAL_EXR_READ_MODE rmode = EXR_MUST_READ_ALL;
    EXR_PROMOTE_READ_CONST_CONTEXT_AND_PART_OR_ERROR (ctxt, part_index);

    if (!cinfo) return pctxt->standard_error (pctxt, EXR_ERR_INVALID_ARGUMENT);
    if (cinfo->packed_size > 0 && !packed_data)
        return pctxt->standard_error (pctxt, EXR_ERR_INVALID_ARGUMENT);

    if (cinfo->idx < 0 || cinfo->idx >= part->chunk_count)
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "invalid chunk index (%d) vs part chunk count %d",
            cinfo->idx,
            part->chunk_count);
    if (cinfo->type != (uint8_t) part->storage_mode)
        return pctxt->report_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "mis-matched storage type for chunk block info");
    if (cinfo->compression != (uint8_t) part->comp_type)
        return pctxt->report_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "mis-matched compression type for chunk block info");

    dataoffset = cinfo->data_offset;
    if (pctxt->file_size > 0 && dataoffset > (uint64_t) pctxt->file_size)
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "chunk block info data offset (%" PRIu64
            ") past end of file (%" PRId64 ")",
            dataoffset,
            pctxt->file_size);

    /* allow a short read if uncompressed */
    if (part->comp_type == EXR_COMPRESSION_NONE) rmode = EXR_ALLOW_SHORT_READ;

    toread = cinfo->packed_size;
    if (toread > 0)
    {
        nread = 0;
        rv    = pctxt->do_read (
            pctxt, packed_data, toread, &dataoffset, &nread, rmode);

        if (rmode == EXR_ALLOW_SHORT_READ && nread < (int64_t) toread)
            memset (
                ((uint8_t*) packed_data) + nread,
                0,
                toread - (uint64_t) (nread));
    }
    else
        rv = EXR_ERR_SUCCESS;

    return rv;
}

/**************************************/

exr_result_t
exr_read_deep_chunk (
    exr_const_context_t     ctxt,
    int                     part_index,
    const exr_chunk_info_t* cinfo,
    void*                   packed_data,
    void*                   sample_data)
{
    exr_result_t                 rv;
    uint64_t                     dataoffset, toread;
    int64_t                      nread;
    enum _INTERNAL_EXR_READ_MODE rmode = EXR_MUST_READ_ALL;
    EXR_PROMOTE_READ_CONST_CONTEXT_AND_PART_OR_ERROR (ctxt, part_index);

    if (!cinfo) return pctxt->standard_error (pctxt, EXR_ERR_INVALID_ARGUMENT);

    if (cinfo->idx < 0 || cinfo->idx >= part->chunk_count)
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "invalid chunk index (%d) vs part chunk count %d",
            cinfo->idx,
            part->chunk_count);
    if (cinfo->type != (uint8_t) part->storage_mode)
        return pctxt->report_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "mis-matched storage type for chunk block info");
    if (cinfo->compression != (uint8_t) part->comp_type)
        return pctxt->report_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "mis-matched compression type for chunk block info");

    if (pctxt->file_size > 0 &&
        cinfo->sample_count_data_offset > (uint64_t) pctxt->file_size)
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "chunk block info sample count offset (%" PRIu64
            ") past end of file (%" PRId64 ")",
            cinfo->sample_count_data_offset,
            pctxt->file_size);

    if (pctxt->file_size > 0 &&
        cinfo->data_offset > (uint64_t) pctxt->file_size)
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "chunk block info data offset (%" PRIu64
            ") past end of file (%" PRId64 ")",
            cinfo->data_offset,
            pctxt->file_size);

    rv = EXR_ERR_SUCCESS;
    if (sample_data && cinfo->sample_count_table_size > 0)
    {
        dataoffset = cinfo->sample_count_data_offset;
        toread     = cinfo->sample_count_table_size;
        nread      = 0;
        rv         = pctxt->do_read (
            pctxt, sample_data, toread, &dataoffset, &nread, rmode);
    }

    if (rv != EXR_ERR_SUCCESS) return rv;

    if (packed_data && cinfo->packed_size > 0)
    {
        dataoffset = cinfo->data_offset;
        toread     = cinfo->packed_size;
        nread      = 0;
        rv         = pctxt->do_read (
            pctxt, packed_data, toread, &dataoffset, &nread, rmode);
    }

    return rv;
}

/**************************************/

/* pull most of the logic to here to avoid having to unlock at every
 * error exit point and re-use mostly shared logic */
static exr_result_t
write_scan_chunk (
    struct _internal_exr_context* pctxt,
    int                           part_index,
    struct _internal_exr_part*    part,
    int                           y,
    const void*                   packed_data,
    uint64_t                      packed_size,
    uint64_t                      unpacked_size,
    const void*                   sample_data,
    uint64_t                      sample_data_size)
{
    exr_result_t rv;
    int32_t      data[3];
    int32_t      psize;
    int          cidx, lpc, miny, wrcnt;
    uint64_t*    ctable;

    if (pctxt->mode != EXR_CONTEXT_WRITING_DATA)
    {
        if (pctxt->mode == EXR_CONTEXT_WRITE)
            return pctxt->standard_error (pctxt, EXR_ERR_HEADER_NOT_WRITTEN);
        return pctxt->standard_error (pctxt, EXR_ERR_NOT_OPEN_WRITE);
    }

    if (part->storage_mode == EXR_STORAGE_TILED ||
        part->storage_mode == EXR_STORAGE_DEEP_TILED)
    {
        return pctxt->standard_error (pctxt, EXR_ERR_SCAN_TILE_MIXEDAPI);
    }

    if (pctxt->cur_output_part != part_index)
        return pctxt->standard_error (pctxt, EXR_ERR_INCORRECT_PART);

    if (packed_size > 0 && !packed_data)
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "Invalid packed data argument size %" PRIu64 " pointer %p",
            (uint64_t) packed_size,
            packed_data);

    if (part->storage_mode != EXR_STORAGE_DEEP_SCANLINE &&
        packed_size > (uint64_t) INT32_MAX)
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "Packed data size %" PRIu64 " too large (max %" PRIu64 ")",
            (uint64_t) packed_size,
            (uint64_t) INT32_MAX);
    psize = (int32_t) packed_size;

    if (part->storage_mode == EXR_STORAGE_DEEP_SCANLINE &&
        (!sample_data || sample_data_size == 0))
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "Invalid sample count data argument size %" PRIu64 " pointer %p",
            (uint64_t) sample_data_size,
            sample_data);

    if (y < part->data_window.min.y || y > part->data_window.max.y)
    {
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "Invalid attempt to write scanlines starting at %d outside range of data window (%d - %d)",
            y,
            part->data_window.min.y,
            part->data_window.max.y);
    }

    lpc  = part->lines_per_chunk;
    cidx = (y - part->data_window.min.y);
    if (lpc > 1) cidx /= lpc;

    //if (part->lineorder == EXR_LINEORDER_DECREASING_Y)
    //    cidx = part->chunk_count - (cidx + 1);

    miny = cidx * lpc + part->data_window.min.y;

    if (y != miny)
    {
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "Attempt to write scanline %d which does not align with y dims (%d) for chunk index (%d)",
            y,
            miny,
            cidx);
    }

    if (cidx < 0 || cidx >= part->chunk_count)
    {
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "Chunk index for scanline %d in chunk %d outside chunk count %d",
            y,
            cidx,
            part->chunk_count);
    }

    if (part->lineorder != EXR_LINEORDER_RANDOM_Y &&
        pctxt->last_output_chunk != (cidx - 1))
    {
        return pctxt->standard_error (pctxt, EXR_ERR_INCORRECT_CHUNK);
    }

    if (pctxt->is_multipart)
    {
        data[0] = part_index;
        data[1] = miny;
        if (part->storage_mode != EXR_STORAGE_DEEP_SCANLINE)
        {
            data[2] = psize;
            wrcnt   = 3;
        }
        else
            wrcnt = 2;
    }
    else
    {
        data[0] = miny;
        if (part->storage_mode != EXR_STORAGE_DEEP_SCANLINE)
        {
            data[1] = psize;
            wrcnt   = 2;
        }
        else
            wrcnt = 1;
    }
    priv_from_native32 (data, wrcnt);

    rv = alloc_chunk_table (pctxt, part, &ctable);
    if (rv != EXR_ERR_SUCCESS) return rv;

    ctable[cidx] = pctxt->output_file_offset;
    rv           = pctxt->do_write (
        pctxt,
        data,
        (uint64_t) (wrcnt) * sizeof (int32_t),
        &(pctxt->output_file_offset));
    if (rv == EXR_ERR_SUCCESS &&
        part->storage_mode == EXR_STORAGE_DEEP_SCANLINE)
    {
        int64_t ddata[3];
        ddata[0] = (int64_t) sample_data_size;
        ddata[1] = (int64_t) packed_size;
        ddata[2] = (int64_t) unpacked_size;
        rv       = pctxt->do_write (
            pctxt, ddata, 3 * sizeof (uint64_t), &(pctxt->output_file_offset));

        if (rv == EXR_ERR_SUCCESS)
            rv = pctxt->do_write (
                pctxt,
                sample_data,
                sample_data_size,
                &(pctxt->output_file_offset));
    }
    if (rv == EXR_ERR_SUCCESS && packed_size > 0)
        rv = pctxt->do_write (
            pctxt, packed_data, packed_size, &(pctxt->output_file_offset));

    if (rv == EXR_ERR_SUCCESS)
    {
        ++(pctxt->output_chunk_count);
        if (pctxt->output_chunk_count == part->chunk_count)
        {
            uint64_t chunkoff = part->chunk_table_offset;

            ++(pctxt->cur_output_part);
            if (pctxt->cur_output_part == pctxt->num_parts)
                pctxt->mode = EXR_CONTEXT_WRITE_FINISHED;
            pctxt->last_output_chunk  = -1;
            pctxt->output_chunk_count = 0;

            priv_from_native64 (ctable, part->chunk_count);
            rv = pctxt->do_write (
                pctxt,
                ctable,
                sizeof (uint64_t) * (uint64_t) (part->chunk_count),
                &chunkoff);
            /* just in case we look at it again? */
            priv_to_native64 (ctable, part->chunk_count);
        }
        else
        {
            pctxt->last_output_chunk = cidx;
        }
    }

    return rv;
}

/**************************************/

exr_result_t
exr_write_scanline_chunk_info (
    exr_context_t ctxt, int part_index, int y, exr_chunk_info_t* cinfo)
{
    exr_attr_box2i_t dw;
    int              lpc, miny, cidx;
    exr_chunk_info_t nil = { 0 };

    EXR_PROMOTE_LOCKED_CONTEXT_AND_PART_OR_ERROR (ctxt, part_index);

    if (!cinfo)
        return EXR_UNLOCK_AND_RETURN_PCTXT (
            pctxt->standard_error (pctxt, EXR_ERR_INVALID_ARGUMENT));

    if (part->storage_mode == EXR_STORAGE_TILED ||
        part->storage_mode == EXR_STORAGE_DEEP_TILED)
    {
        return EXR_UNLOCK_AND_RETURN_PCTXT (
            pctxt->standard_error (pctxt, EXR_ERR_SCAN_TILE_MIXEDAPI));
    }

    if (pctxt->mode != EXR_CONTEXT_WRITING_DATA)
    {
        if (pctxt->mode == EXR_CONTEXT_WRITE)
            return EXR_UNLOCK_AND_RETURN_PCTXT (
                pctxt->standard_error (pctxt, EXR_ERR_HEADER_NOT_WRITTEN));
        return EXR_UNLOCK_AND_RETURN_PCTXT (
            pctxt->standard_error (pctxt, EXR_ERR_NOT_OPEN_WRITE));
    }

    dw = part->data_window;
    if (y < dw.min.y || y > dw.max.y)
    {
        return EXR_UNLOCK_AND_RETURN_PCTXT (pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "Invalid request for scanline %d outside range of data window (%d - %d)",
            y,
            dw.min.y,
            dw.max.y));
    }

    lpc  = part->lines_per_chunk;
    cidx = (y - dw.min.y);
    if (lpc > 1) cidx /= lpc;

    //if (part->lineorder == EXR_LINEORDER_DECREASING_Y)
    //    cidx = part->chunk_count - (cidx + 1);
    miny = cidx * lpc + dw.min.y;

    if (cidx < 0 || cidx >= part->chunk_count)
    {
        return EXR_UNLOCK_AND_RETURN_PCTXT (pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "Invalid request for scanline %d in chunk %d outside chunk count %d",
            y,
            cidx,
            part->chunk_count));
    }

    *cinfo             = nil;
    cinfo->idx         = cidx;
    cinfo->type        = (uint8_t) part->storage_mode;
    cinfo->compression = (uint8_t) part->comp_type;
    cinfo->start_x     = dw.min.x;
    cinfo->start_y     = miny;
    cinfo->width       = dw.max.x - dw.min.x + 1;
    cinfo->height      = lpc;
    if (miny < dw.min.y)
    {
        cinfo->start_y = dw.min.y;
        cinfo->height -= (dw.min.y - miny);
    }
    else if ((miny + lpc) > dw.max.y)
    {
        cinfo->height = (dw.max.y - miny + 1);
    }
    cinfo->level_x = 0;
    cinfo->level_y = 0;

    cinfo->sample_count_data_offset = 0;
    cinfo->sample_count_table_size  = 0;
    cinfo->data_offset              = 0;
    cinfo->packed_size              = 0;
    cinfo->unpacked_size            = part->unpacked_size_per_chunk;

    return EXR_UNLOCK_AND_RETURN_PCTXT (EXR_ERR_SUCCESS);
}

/**************************************/

exr_result_t
exr_write_tile_chunk_info (
    exr_context_t     ctxt,
    int               part_index,
    int               tilex,
    int               tiley,
    int               levelx,
    int               levely,
    exr_chunk_info_t* cinfo)
{
    exr_result_t               rv;
    int                        cidx;
    const exr_attr_chlist_t*   chanlist;
    const exr_attr_tiledesc_t* tiledesc;
    int                        tilew, tileh;
    uint64_t                   unpacksize = 0;
    exr_chunk_info_t           nil        = { 0 };

    EXR_PROMOTE_LOCKED_CONTEXT_AND_PART_OR_ERROR (ctxt, part_index);

    if (!cinfo)
        return EXR_UNLOCK_AND_RETURN_PCTXT (
            pctxt->standard_error (pctxt, EXR_ERR_INVALID_ARGUMENT));

    if (tilex < 0 || tiley < 0 || levelx < 0 || levely < 0)
        return EXR_UNLOCK_AND_RETURN_PCTXT (
            pctxt->standard_error (pctxt, EXR_ERR_INVALID_ARGUMENT));
    if (part->storage_mode == EXR_STORAGE_SCANLINE ||
        part->storage_mode == EXR_STORAGE_DEEP_SCANLINE)
    {
        return EXR_UNLOCK_AND_RETURN_PCTXT (
            pctxt->standard_error (pctxt, EXR_ERR_TILE_SCAN_MIXEDAPI));
    }

    if (!part->tiles || part->num_tile_levels_x <= 0 ||
        part->num_tile_levels_y <= 0 || !part->tile_level_tile_count_x ||
        !part->tile_level_tile_count_y)
    {
        return EXR_UNLOCK_WRITE_AND_RETURN_PCTXT (pctxt->report_error (
            pctxt, EXR_ERR_MISSING_REQ_ATTR, "Tile data missing or corrupt"));
    }

    if (pctxt->mode != EXR_CONTEXT_WRITING_DATA)
    {
        if (pctxt->mode == EXR_CONTEXT_WRITE)
            return EXR_UNLOCK_AND_RETURN_PCTXT (
                pctxt->standard_error (pctxt, EXR_ERR_HEADER_NOT_WRITTEN));
        return EXR_UNLOCK_AND_RETURN_PCTXT (
            pctxt->standard_error (pctxt, EXR_ERR_NOT_OPEN_WRITE));
    }

    tiledesc = part->tiles->tiledesc;
    tilew    = part->tile_level_tile_size_x[levelx];
    if (tiledesc->x_size < (uint32_t) tilew) tilew = (int) tiledesc->x_size;
    tileh = part->tile_level_tile_size_y[levely];
    if (tiledesc->y_size < (uint32_t) tileh) tileh = (int) tiledesc->y_size;

    if (((int64_t) (tilex) * (int64_t) (tilew) + (int64_t) (tilew) +
         (int64_t) (part->data_window.min.x) - 1) >
        (int64_t) (part->data_window.max.x))
    {
        int64_t sz = (int64_t) (part->data_window.max.x) -
                     (int64_t) (part->data_window.min.x) + 1;
        tilew = (int) (sz - ((int64_t) (tilex) * (int64_t) (tilew)));
    }

    if (((int64_t) (tiley) * (int64_t) (tileh) + (int64_t) (tileh) +
         (int64_t) (part->data_window.min.y) - 1) >
        (int64_t) (part->data_window.max.y))
    {
        int64_t sz = (int64_t) (part->data_window.max.y) -
                     (int64_t) (part->data_window.min.y) + 1;
        tileh = (int) (sz - ((int64_t) (tiley) * (int64_t) (tileh)));
    }

    cidx = 0;
    rv   = compute_tile_chunk_off (
        pctxt, part, tilex, tiley, levelx, levely, &cidx);
    if (rv != EXR_ERR_SUCCESS) return EXR_UNLOCK_AND_RETURN_PCTXT (rv);

    *cinfo             = nil;
    cinfo->idx         = cidx;
    cinfo->type        = (uint8_t) part->storage_mode;
    cinfo->compression = (uint8_t) part->comp_type;
    cinfo->start_x     = tilex;
    cinfo->start_y     = tiley;
    cinfo->height      = tileh;
    cinfo->width       = tilew;
    if (levelx > 255 || levely > 255)
        return pctxt->print_error (
            pctxt,
            EXR_ERR_ATTR_SIZE_MISMATCH,
            "Unable to represent tile level %d, %d in chunk structure",
            levelx,
            levely);

    cinfo->level_x = (uint8_t) levelx;
    cinfo->level_y = (uint8_t) levely;

    chanlist = part->channels->chlist;
    for (int c = 0; c < chanlist->num_channels; ++c)
    {
        const exr_attr_chlist_entry_t* curc = (chanlist->entries + c);
        unpacksize += (uint64_t) (tilew) * (uint64_t) (tileh) *
                      (uint64_t) ((curc->pixel_type == EXR_PIXEL_HALF) ? 2 : 4);
    }

    cinfo->sample_count_data_offset = 0;
    cinfo->sample_count_table_size  = 0;
    cinfo->data_offset              = 0;
    cinfo->packed_size              = 0;
    cinfo->unpacked_size            = unpacksize;

    return EXR_UNLOCK_AND_RETURN_PCTXT (EXR_ERR_SUCCESS);
}

/**************************************/

exr_result_t
exr_write_scanline_chunk (
    exr_context_t ctxt,
    int           part_index,
    int           y,
    const void*   packed_data,
    uint64_t      packed_size)
{
    exr_result_t rv;
    EXR_PROMOTE_LOCKED_CONTEXT_AND_PART_OR_ERROR (ctxt, part_index);

    if (part->storage_mode == EXR_STORAGE_DEEP_SCANLINE)
        return EXR_UNLOCK_AND_RETURN_PCTXT (
            pctxt->standard_error (pctxt, EXR_ERR_USE_SCAN_DEEP_WRITE));

    rv = write_scan_chunk (
        pctxt, part_index, part, y, packed_data, packed_size, 0, NULL, 0);
    return EXR_UNLOCK_AND_RETURN_PCTXT (rv);
}

/**************************************/

exr_result_t
exr_write_deep_scanline_chunk (
    exr_context_t ctxt,
    int           part_index,
    int           y,
    const void*   packed_data,
    uint64_t      packed_size,
    uint64_t      unpacked_size,
    const void*   sample_data,
    uint64_t      sample_data_size)
{
    exr_result_t rv;
    EXR_PROMOTE_LOCKED_CONTEXT_AND_PART_OR_ERROR (ctxt, part_index);

    if (part->storage_mode == EXR_STORAGE_SCANLINE)
        return EXR_UNLOCK_AND_RETURN_PCTXT (
            pctxt->standard_error (pctxt, EXR_ERR_USE_SCAN_NONDEEP_WRITE));

    rv = write_scan_chunk (
        pctxt,
        part_index,
        part,
        y,
        packed_data,
        packed_size,
        unpacked_size,
        sample_data,
        sample_data_size);
    return EXR_UNLOCK_AND_RETURN_PCTXT (rv);
}

/**************************************/

/* pull most of the logic to here to avoid having to unlock at every
 * error exit point and re-use mostly shared logic */
static exr_result_t
write_tile_chunk (
    struct _internal_exr_context* pctxt,
    int                           part_index,
    struct _internal_exr_part*    part,
    int                           tilex,
    int                           tiley,
    int                           levelx,
    int                           levely,
    const void*                   packed_data,
    uint64_t                      packed_size,
    uint64_t                      unpacked_size,
    const void*                   sample_data,
    uint64_t                      sample_data_size)
{
    exr_result_t rv;
    int32_t      data[6];
    int32_t      psize;
    int          cidx, wrcnt;
    uint64_t*    ctable;

    if (pctxt->mode != EXR_CONTEXT_WRITING_DATA)
    {
        if (pctxt->mode == EXR_CONTEXT_WRITE)
            return pctxt->standard_error (pctxt, EXR_ERR_HEADER_NOT_WRITTEN);
        return pctxt->standard_error (pctxt, EXR_ERR_NOT_OPEN_WRITE);
    }

    if (part->storage_mode == EXR_STORAGE_SCANLINE ||
        part->storage_mode == EXR_STORAGE_DEEP_SCANLINE)
    {
        return pctxt->standard_error (pctxt, EXR_ERR_TILE_SCAN_MIXEDAPI);
    }

    if (pctxt->cur_output_part != part_index)
        return pctxt->standard_error (pctxt, EXR_ERR_INCORRECT_PART);

    if (!packed_data || packed_size == 0)
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "Invalid packed data argument size %" PRIu64 " pointer %p",
            (uint64_t) packed_size,
            packed_data);

    if (part->storage_mode != EXR_STORAGE_DEEP_TILED &&
        packed_size > (uint64_t) INT32_MAX)
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "Packed data size %" PRIu64 " too large (max %" PRIu64 ")",
            (uint64_t) packed_size,
            (uint64_t) INT32_MAX);
    psize = (int32_t) packed_size;

    if (part->storage_mode == EXR_STORAGE_DEEP_TILED &&
        (!sample_data || sample_data_size == 0))
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "Invalid sample count data argument size %" PRIu64 " pointer %p",
            (uint64_t) sample_data_size,
            sample_data);

    if (!part->tiles || part->num_tile_levels_x <= 0 ||
        part->num_tile_levels_y <= 0 || !part->tile_level_tile_count_x ||
        !part->tile_level_tile_count_y)
    {
        return pctxt->print_error (
            pctxt,
            EXR_ERR_MISSING_REQ_ATTR,
            "Attempting to write tiled part, but tile data missing or corrupt");
    }

    cidx = -1;
    rv   = compute_tile_chunk_off (
        pctxt, part, tilex, tiley, levelx, levely, &cidx);
    if (rv != EXR_ERR_SUCCESS) return rv;

    if (cidx < 0 || cidx >= part->chunk_count)
    {
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INVALID_ARGUMENT,
            "Chunk index for tile (%d, %d) at level (%d, %d) %d outside chunk count %d",
            tilex,
            tiley,
            levelx,
            levely,
            cidx,
            part->chunk_count);
    }

    if (part->lineorder != EXR_LINEORDER_RANDOM_Y &&
        pctxt->last_output_chunk != (cidx - 1))
    {
        return pctxt->print_error (
            pctxt,
            EXR_ERR_INCORRECT_CHUNK,
            "Chunk index %d is not the next chunk to be written (last %d)",
            cidx,
            pctxt->last_output_chunk);
    }

    wrcnt = 0;
    if (pctxt->is_multipart) { data[wrcnt++] = part_index; }
    data[wrcnt++] = tilex;
    data[wrcnt++] = tiley;
    data[wrcnt++] = levelx;
    data[wrcnt++] = levely;
    if (part->storage_mode != EXR_STORAGE_DEEP_TILED) data[wrcnt++] = psize;

    priv_from_native32 (data, wrcnt);

    rv = alloc_chunk_table (pctxt, part, &ctable);
    if (rv != EXR_ERR_SUCCESS) return rv;

    ctable[cidx] = pctxt->output_file_offset;
    rv           = pctxt->do_write (
        pctxt,
        data,
        (uint64_t) (wrcnt) * sizeof (int32_t),
        &(pctxt->output_file_offset));
    if (rv == EXR_ERR_SUCCESS && part->storage_mode == EXR_STORAGE_DEEP_TILED)
    {
        int64_t ddata[3];
        ddata[0] = (int64_t) sample_data_size;
        ddata[1] = (int64_t) packed_size;
        ddata[2] = (int64_t) unpacked_size;
        rv       = pctxt->do_write (
            pctxt, ddata, 3 * sizeof (uint64_t), &(pctxt->output_file_offset));

        if (rv == EXR_ERR_SUCCESS)
            rv = pctxt->do_write (
                pctxt,
                sample_data,
                sample_data_size,
                &(pctxt->output_file_offset));
    }
    if (rv == EXR_ERR_SUCCESS)
        rv = pctxt->do_write (
            pctxt, packed_data, packed_size, &(pctxt->output_file_offset));

    if (rv == EXR_ERR_SUCCESS)
    {
        ++(pctxt->output_chunk_count);
        if (pctxt->output_chunk_count == part->chunk_count)
        {
            uint64_t chunkoff = part->chunk_table_offset;

            ++(pctxt->cur_output_part);
            if (pctxt->cur_output_part == pctxt->num_parts)
                pctxt->mode = EXR_CONTEXT_WRITE_FINISHED;
            pctxt->last_output_chunk  = -1;
            pctxt->output_chunk_count = 0;

            priv_from_native64 (ctable, part->chunk_count);
            rv = pctxt->do_write (
                pctxt,
                ctable,
                sizeof (uint64_t) * (uint64_t) (part->chunk_count),
                &chunkoff);
            /* just in case we look at it again? */
            priv_to_native64 (ctable, part->chunk_count);
        }
        else
        {
            pctxt->last_output_chunk = cidx;
        }
    }

    return rv;
}

/**************************************/

exr_result_t
exr_write_tile_chunk (
    exr_context_t ctxt,
    int           part_index,
    int           tilex,
    int           tiley,
    int           levelx,
    int           levely,
    const void*   packed_data,
    uint64_t      packed_size)
{
    exr_result_t rv;
    EXR_PROMOTE_LOCKED_CONTEXT_AND_PART_OR_ERROR (ctxt, part_index);

    if (part->storage_mode == EXR_STORAGE_DEEP_TILED)
        return EXR_UNLOCK_AND_RETURN_PCTXT (
            pctxt->standard_error (pctxt, EXR_ERR_USE_TILE_DEEP_WRITE));

    rv = write_tile_chunk (
        pctxt,
        part_index,
        part,
        tilex,
        tiley,
        levelx,
        levely,
        packed_data,
        packed_size,
        0,
        NULL,
        0);
    return EXR_UNLOCK_AND_RETURN_PCTXT (rv);
}

/**************************************/

exr_result_t
exr_write_deep_tile_chunk (
    exr_context_t ctxt,
    int           part_index,
    int           tilex,
    int           tiley,
    int           levelx,
    int           levely,
    const void*   packed_data,
    uint64_t      packed_size,
    uint64_t      unpacked_size,
    const void*   sample_data,
    uint64_t      sample_data_size)
{
    exr_result_t rv;
    EXR_PROMOTE_LOCKED_CONTEXT_AND_PART_OR_ERROR (ctxt, part_index);

    if (part->storage_mode == EXR_STORAGE_TILED)
        return EXR_UNLOCK_AND_RETURN_PCTXT (
            pctxt->standard_error (pctxt, EXR_ERR_USE_TILE_NONDEEP_WRITE));

    rv = write_tile_chunk (
        pctxt,
        part_index,
        part,
        tilex,
        tiley,
        levelx,
        levely,
        packed_data,
        packed_size,
        unpacked_size,
        sample_data,
        sample_data_size);
    return EXR_UNLOCK_AND_RETURN_PCTXT (rv);
}

/**************************************/

exr_result_t
internal_validate_next_chunk (
    exr_encode_pipeline_t*              encode,
    const struct _internal_exr_context* pctxt,
    const struct _internal_exr_part*    part)
{
    exr_result_t rv = EXR_ERR_SUCCESS;
    int          cidx, lpc;

    if (pctxt->cur_output_part != encode->part_index)
        return pctxt->standard_error (pctxt, EXR_ERR_INCORRECT_PART);

    cidx = -1;

    if (part->storage_mode == EXR_STORAGE_TILED ||
        part->storage_mode == EXR_STORAGE_DEEP_TILED)
    {
        rv = compute_tile_chunk_off (
            pctxt,
            part,
            encode->chunk.start_x,
            encode->chunk.start_y,
            encode->chunk.level_x,
            encode->chunk.level_y,
            &cidx);
    }
    else
    {
        lpc  = part->lines_per_chunk;
        cidx = (encode->chunk.start_y - part->data_window.min.y);
        if (lpc > 1) cidx /= lpc;

        //if (part->lineorder == EXR_LINEORDER_DECREASING_Y)
        //{
        //    cidx = part->chunk_count - (cidx + 1);
        //}
    }

    if (rv == EXR_ERR_SUCCESS)
    {
        if (cidx < 0 || cidx >= part->chunk_count)
        {
            rv = pctxt->print_error (
                pctxt,
                EXR_ERR_INVALID_ARGUMENT,
                "Chunk index for scanline %d in chunk %d outside chunk count %d",
                encode->chunk.start_y,
                cidx,
                part->chunk_count);
        }
        else if (
            part->lineorder != EXR_LINEORDER_RANDOM_Y &&
            pctxt->last_output_chunk != (cidx - 1))
        {
            rv = pctxt->print_error (
                pctxt,
                EXR_ERR_INCORRECT_CHUNK,
                "Attempt to write chunk %d, but last output chunk is %d",
                cidx,
                pctxt->last_output_chunk);
        }
    }
    return rv;
}