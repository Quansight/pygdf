/*
 * Copyright (c) 2018, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
	 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cuda_runtime.h>
#include "io_uncomp.h"
#include "rmm/rmm.h"
#include <string.h> // memset
#include <zlib.h> // uncompress

#define GZ_FLG_FTEXT    0x01    // ASCII text hint
#define GZ_FLG_FHCRC    0x02    // Header CRC present
#define GZ_FLG_FEXTRA   0x04    // Extra fields present
#define GZ_FLG_FNAME    0x08    // Original file name present
#define GZ_FLG_FCOMMENT 0x10    // Comment present

#pragma pack(push, 1)

struct gz_file_header_s
{
    uint8_t id1;        // 0x1f
    uint8_t id2;        // 0x8b
    uint8_t comp_mthd;  // compression method (0-7=reserved, 8=deflate)
    uint8_t flags;      // flags (GZ_FLG_XXX)
    uint8_t mtime[4];   // If non-zero: modification time (Unix format)
    uint8_t xflags;     // Extra compressor-specific flags
    uint8_t os;         // OS id
};

struct zip_eocd_s // end of central directory
{
    uint32_t sig;           // 0x06054b50
    uint16_t disk_id;       // number of this disk
    uint16_t start_disk;    // number of the disk with the start of the central directory
    uint16_t num_entries;   // number of entries in the central dir on this disk
    uint16_t total_entries; // total number of entries in the central dir
    uint32_t cdir_size;     // size of the central directory
    uint32_t cdir_offset;   // offset of start of central directory with respect to the starting disk number
    //uint16_t comment_len;   // comment length (excluded from struct)
};


struct zip64_eocdl // end of central dir locator
{
    uint32_t sig;           // 0x07064b50
    uint32_t disk_start;    // number of the disk with the start of the zip64 end of central directory
    uint64_t eocdr_ofs;     // relative offset of the zip64 end of central directory record
    uint32_t num_disks;     // total number of disks
};

struct zip_cdfh_s    // central directory file header
{
    uint32_t sig;           // 0x02014b50
    uint16_t ver;           // version made by
    uint16_t min_ver;       // version needed to extract
    uint16_t gp_flags;      // general purpose bit flag
    uint16_t comp_method;   // compression method
    uint16_t file_time;     // last mod file time
    uint16_t file_date;     // last mod file date
    uint32_t crc32;         // crc - 32
    uint32_t comp_size;     // compressed size
    uint32_t uncomp_size;   // uncompressed size
    uint16_t fname_len;     // filename length
    uint16_t extra_len;     // extra field length
    uint16_t comment_len;   // file comment length
    uint16_t start_disk;    // disk number start
    uint16_t int_fattr;     // internal file attributes
    uint32_t ext_fattr;     // external file attributes
    uint32_t hdr_ofs;       // relative offset of local header
};

struct zip_lfh_s
{
    uint32_t sig;           // 0x04034b50
    uint16_t ver;           // version needed to extract
    uint16_t gp_flags;      // general purpose bit flag
    uint16_t comp_method;   // compression method
    uint16_t file_time;     // last mod file time
    uint16_t file_date;     // last mod file date
    uint32_t crc32;         // crc - 32
    uint32_t comp_size;     // compressed size
    uint32_t uncomp_size;   // uncompressed size
    uint16_t fname_len;     // filename length
    uint16_t extra_len;     // extra field length
};

#pragma pack(pop)

struct gz_archive_s
{
    const gz_file_header_s *fhdr;
    uint16_t hcrc16;    // header crc16 if present
    uint16_t xlen;
    const uint8_t *fxtra;     // xlen bytes (optional)
    const uint8_t *fname;     // zero-terminated original filename if present
    const uint8_t *fcomment;  // zero-terminated comment if present
    const uint8_t *comp_data; // compressed data
    size_t comp_len;        // Compressed data length
    uint32_t crc32;     // CRC32 of uncompressed data
    uint32_t isize;     // Input size modulo 2^32
};

struct zip_archive_s
{
    const zip_eocd_s *eocd;     // end of central directory
    const zip64_eocdl *eocdl;   // end of central dir locator (optional)
    const zip_cdfh_s *cdfh;     // start of central directory file headers
};


bool ParseGZArchive(gz_archive_s *dst, const uint8_t *raw, size_t len)
{
    const gz_file_header_s *fhdr;

    if (!dst)
        return false;
    memset(dst, 0, sizeof(gz_archive_s));
    if (len < sizeof(gz_file_header_s) + 8)
        return false;
    fhdr = (gz_file_header_s *)raw;
    if (fhdr->id1 != 0x1f || fhdr->id2 != 0x8b)
        return false;
    dst->fhdr = fhdr;
    raw += sizeof(gz_file_header_s);
    len -= sizeof(gz_file_header_s);
    if (fhdr->flags & GZ_FLG_FEXTRA)
    {
        uint32_t xlen;

        if (len < 2)
            return false;
        xlen = raw[0] | (raw[1] << 8);
        raw += 2;
        len -= 2;
        if (len < xlen)
            return false;
        dst->xlen = (uint16_t)xlen;
        dst->fxtra = raw;
        raw += xlen;
        len -= xlen;
    }
    if (fhdr->flags & GZ_FLG_FNAME)
    {
        size_t l = 0;
        uint8_t c;
        do
        {
            if (l >= len)
                return false;
            c = raw[l];
            l++;
        } while (c != 0);
        dst->fname = raw;
        raw += l;
        len -= l;
    }
    if (fhdr->flags & GZ_FLG_FCOMMENT)
    {
        size_t l = 0;
        uint8_t c;
        do
        {
            if (l >= len)
                return false;
            c = raw[l];
            l++;
        } while (c != 0);
        dst->fcomment = raw;
        raw += l;
        len -= l;
    }
    if (fhdr->flags & GZ_FLG_FHCRC)
    {
        if (len < 2)
            return false;
        dst->hcrc16 = raw[0] | (raw[1] << 8);
        raw += 2;
        len -= 2;
    }
    if (len < 8)
        return false;
    dst->crc32 = raw[len - 8] | (raw[len - 7] << 8) | (raw[len - 6] << 16) | (raw[len - 5] << 24);
    dst->isize = raw[len - 4] | (raw[len - 3] << 8) | (raw[len - 2] << 16) | (raw[len - 1] << 24);
    len -= 8;
    dst->comp_data = raw;
    dst->comp_len = len;
    return (fhdr->comp_mthd == 8 && len > 0);
}


bool OpenZipArchive(zip_archive_s *dst, const uint8_t *raw, size_t len)
{
    memset(dst, 0, sizeof(zip_archive_s));
    // Find the end of central directory
    if (len >= sizeof(zip_eocd_s) + 2)
    {
        for (size_t i = len - sizeof(zip_eocd_s) - 2; i + sizeof(zip_eocd_s) + 2 + 0xffff >= len; i--)
        {
            const zip_eocd_s *eocd = (zip_eocd_s *)(raw + i);
            if (eocd->sig == 0x06054b50
             && eocd->disk_id == eocd->start_disk // multi-file archives not supported
             && eocd->num_entries == eocd->total_entries
             && eocd->cdir_size >= sizeof(zip_cdfh_s) * eocd->num_entries
             && eocd->cdir_offset < len
             && i + *(const uint16_t *)(eocd + 1) <= len)
            {
                const zip_cdfh_s *cdfh = (const zip_cdfh_s *)(raw + eocd->cdir_offset);
                dst->eocd = eocd;
                if (i >= sizeof(zip64_eocdl))
                {
                    const zip64_eocdl *eocdl = (const zip64_eocdl *)(raw + i - sizeof(zip64_eocdl));
                    if (eocdl->sig == 0x07064b50)
                    {
                        dst->eocdl = eocdl;
                    }
                }
                // Start of central directory
                if (cdfh->sig == 0x02014b50)
                {
                    dst->cdfh = cdfh;
                }
            }
        }
    }
    return (dst->eocd && dst->cdfh);
}


int cpu_inflate(uint8_t *uncomp_data, size_t *destLen, const uint8_t *comp_data, size_t comp_len)
{
    int zerr;
    z_stream strm;

    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef *)comp_data;
    strm.avail_in = comp_len;
    strm.total_in = 0;
    strm.next_out = uncomp_data;
    strm.avail_out = *destLen;
    strm.total_out = 0;
    zerr = inflateInit2(&strm, -15); // -15 for raw data without GZIP headers
    if (zerr != 0)
    {
        *destLen = 0;
        return zerr;
    }
    zerr = inflate(&strm, Z_FINISH);
    *destLen = strm.total_out;
    inflateEnd(&strm);
    return (zerr == Z_STREAM_END) ? Z_OK : zerr;
}


/* --------------------------------------------------------------------------*/
/** 
 * @Brief Uncompresses a gzip/zip/bzip2/xz file stored in system memory.
 * The result is allocated and stored in a vector.
 * If the function call fails, the output vector is empty.
 * 
 * @param src[in] Pointer to the compressed data in system memory
 * @param src_size[in] The size of the compressed data, in bytes
 * @param strm_type[in] Type of compression of the input data
 * @param dst[out] Vector containing the uncompressed output
 * 
 * @Returns gdf_error with error code on failure, otherwise GDF_SUCCESS
 */
/* ----------------------------------------------------------------------------*/
gdf_error io_uncompress_single_h2d(const void *src, gdf_size_type src_size, int strm_type, std::vector<char>& dst)
{
    const uint8_t *raw = (const uint8_t *)src;
    const uint8_t *comp_data = nullptr;
    size_t comp_len = 0;
    size_t uncomp_len = 0;
    cudaStream_t strm = (cudaStream_t)0;
    rmmError_t rmmErr;
    cudaError_t cu_err;

    if (!(src && src_size))
    {
        return GDF_INVALID_API_CALL;
    }
    switch(strm_type)
    {
    case IO_UNCOMP_STREAM_TYPE_INFER:
    case IO_UNCOMP_STREAM_TYPE_GZIP:
        {
            gz_archive_s gz;
            if (ParseGZArchive(&gz, raw, src_size))
            {
                strm_type = IO_UNCOMP_STREAM_TYPE_GZIP;
                comp_data = gz.comp_data;
                comp_len = gz.comp_len;
                uncomp_len = gz.isize;
            }
            if (strm_type != IO_UNCOMP_STREAM_TYPE_INFER)
                break; // Fall through for INFER
        }
    case IO_UNCOMP_STREAM_TYPE_ZIP:
        {
            zip_archive_s za;
            if (OpenZipArchive(&za, raw, src_size))
            {
                size_t cdfh_ofs = 0;
                for (int i = 0; i < za.eocd->num_entries; i++)
                {
                    const zip_cdfh_s *cdfh = (const zip_cdfh_s *)(((const uint8_t *)za.cdfh) + cdfh_ofs);
                    int cdfh_len = sizeof(zip_cdfh_s) + cdfh->fname_len + cdfh->extra_len + cdfh->comment_len;
                    if (cdfh_ofs + cdfh_len > za.eocd->cdir_size || cdfh->sig != 0x02014b50)
                    {
                        // Bad cdir
                        break;
                    }
                    // For now, only accept with non-zero file sizes and v2.0 DEFLATE
                    if (cdfh->min_ver <= 20 && cdfh->comp_method == 8 && cdfh->comp_size > 0 && cdfh->uncomp_size > 0)
                    {
                        size_t lfh_ofs = cdfh->hdr_ofs;
                        const zip_lfh_s *lfh = (const zip_lfh_s *)(raw + lfh_ofs);
                        if (lfh_ofs + sizeof(zip_lfh_s) <= src_size
                         && lfh->sig == 0x04034b50
                         && lfh_ofs + sizeof(zip_lfh_s) + lfh->fname_len + lfh->extra_len <= src_size)
                        {
                            if (lfh->ver <= 20 && lfh->comp_method == 8 && lfh->comp_size > 0 && lfh->uncomp_size > 0)
                            {
                                size_t file_start = lfh_ofs + sizeof(zip_lfh_s) + lfh->fname_len + lfh->extra_len;
                                size_t file_end = file_start + lfh->comp_size;
                                if (file_end <= src_size)
                                {
                                    // Pick the first valid file of non-zero size (only 1 file expected in archive)
                                    strm_type = IO_UNCOMP_STREAM_TYPE_ZIP;
                                    comp_data = raw + file_start;
                                    comp_len = lfh->comp_size;
                                    uncomp_len = lfh->uncomp_size;
                                    break;
                                }
                            }
                        }
                    }
                    cdfh_ofs += cdfh_len;
                }
            }
            if (strm_type != IO_UNCOMP_STREAM_TYPE_INFER)
                break; // Fall through for INFER
        default:
            // Unsupported format
            break;
        }
    }
    if (!comp_data || comp_len <= 0)
        return GDF_UNSUPPORTED_DTYPE;
    if (uncomp_len <= 0)
    {
        uncomp_len = comp_len * 4 + 4096; // In case uncompressed size isn't known in advance, assume ~4:1 compression for initial size
    }

    if (strm_type == IO_UNCOMP_STREAM_TYPE_GZIP || strm_type == IO_UNCOMP_STREAM_TYPE_ZIP)
    {
        // INFLATE
    	dst.resize(uncomp_len);
        size_t zdestLen = uncomp_len;
        int zerr = cpu_inflate((uint8_t*)dst.data(), &zdestLen, comp_data, comp_len);
        if (zerr != 0 || zdestLen != uncomp_len)
        {
    		dst.resize(0);
            return GDF_FILE_ERROR;
        }
    }
    else
    {
        return GDF_UNSUPPORTED_DTYPE;
    }

    return GDF_SUCCESS;
}



