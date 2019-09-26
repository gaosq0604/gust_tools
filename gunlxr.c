/*
  Lxr_Decrypt - Archive unpacker for Gust (Koei/Tecmo) .elixir[.gz] files
  Copyright © 2019 VitaSmith

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "util.h"

#define MINIZ_NO_STDIO
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_TIME
#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_MALLOC
#include "miniz_tinfl.h"

#define ELIXIR_MAGIC            0x45415243     // "CRAE"
#define DEFAULT_CHUNK_SIZE      0x4000

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t total_size;
    uint32_t table_offset;
    uint32_t table_size;
    uint32_t nb_files;
    uint32_t unknown;           // Can be 0x0 or 0xA
} lxr_header;

typedef struct {
    uint32_t offset;
    uint32_t size;
    char     filename[0x30];
} lxr_entry;
#pragma pack(pop)

int main(int argc, char** argv)
{
    uint8_t* buf = NULL;
    uint32_t zsize;
    int r = -1;
    const char* app_name = basename(argv[0]);
    if (argc != 2) {
        printf("%s (c) 2019 VitaSmith\n\nUsage: %s <Gust elixir[.gz] file>\n\n"
            "Dumps the elixir format archive to the current directory.\n",
            app_name, app_name);
        return 0;
    }

    // Don't bother checking for case or if these extensions are really at the end
    if (strstr(argv[1], ".elixir") == NULL) {
        fprintf(stderr, "ERROR: File should have a '.elixir[.gz]' extension\n");
        return -1;
    }
    char* is_compressed = strstr(argv[1], ".gz");

    FILE* src = fopen(argv[1], "rb");
    if (src == NULL) {
        fprintf(stderr, "ERROR: Can't open elixir file '%s'", argv[1]);
        return -1;
    }

    // Some elixir.gz files are actually uncompressed versions
    if (fread(&zsize, sizeof(zsize), 1, src) != 1) {
        fprintf(stderr, "ERROR: Can't read from elixir file '%s'", argv[1]);
        goto out;
    }
    if (zsize == ELIXIR_MAGIC)
        is_compressed = NULL;
    fseek(src, 0L, SEEK_END);
    size_t lxr_size = ftell(src);
    fseek(src, 0L, SEEK_SET);

    if (is_compressed != NULL) {
        // Expect the decompressed data not to be larger than 32x the file size
        lxr_size *= 32;
        buf = malloc(lxr_size);
        if (buf == NULL)
            goto out;
        size_t pos = 0;
        while (1) {
            if (fread(&zsize, sizeof(zsize), 1, src) != 1) {
                fprintf(stderr, "ERROR: Can't read compressed stream size");
                goto out;
            }
            if (zsize == 0)
                break;
            uint8_t* zbuf = malloc(zsize);
            if (zbuf == NULL)
                goto out;
            if (fread(zbuf, 1, zsize, src) != zsize) {
                fprintf(stderr, "ERROR: Can't read compressed stream");
                free(zbuf);
                goto out;
            }

            // TODO: realloc if we don't have enough buffer
            size_t s = tinfl_decompress_mem_to_mem(&buf[pos], lxr_size - pos, zbuf, zsize,
                TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_COMPUTE_ADLER32);
            if (s == 0) {
                fprintf(stderr, "ERROR: Can't decompress stream");
                goto out;
            }
            pos += s;
        } while (zsize != 0);
        lxr_size = pos;

//#define DECOMPRESS_ONLY
#ifdef DECOMPRESS_ONLY
        FILE* dst = NULL;
        *is_compressed = 0;
        dst = fopen(argv[1], "wb");
        if (dst == NULL) {
            fprintf(stderr, "ERROR: Can't create file '%s'\n", argv[1]);
            goto out;
        }
        if (fwrite(buf, 1, lxr_size, dst) != lxr_size) {
            fprintf(stderr, "ERROR: Can't write file '%s'\n", argv[1]);
            fclose(dst);
            goto out;
        }
        printf("%08x %s\n", (uint32_t)lxr_size, argv[1]);
        fclose(dst);
        r = 0;
        goto out;
#endif
    } else {
        buf = malloc(lxr_size);
        if (buf == NULL)
            goto out;
        if (fread(buf, 1, lxr_size, src) != lxr_size) {
            fprintf(stderr, "ERROR: Can't read uncompressed data");
            goto out;
        }
    }

    // Now that have an uncompressed .elixir file, extract the files
    assert(strstr(argv[1], ".elixir") != NULL);
    *(strstr(argv[1], ".elixir")) = 0;
    if (!create_path(argv[1]))
        goto out;

    lxr_header* hdr = (lxr_header*)buf;
    if (hdr->magic != ELIXIR_MAGIC) {
        fprintf(stderr, "ERROR: Not an elixir file (bad magic)\n");
        goto out;
    }
    if (hdr->version != 1) {
        fprintf(stderr, "ERROR: Invalid elixir version (0x%08X)\n", hdr->version);
        goto out;
    }
    if (sizeof(lxr_header) + hdr->nb_files * sizeof(lxr_entry) + hdr->total_size != lxr_size) {
        fprintf(stderr, "ERROR: File size mismatch\n");
        goto out;
    }

    char path[256];
    printf("OFFSET   SIZE     NAME\n");
    for (uint32_t i = 0; i < hdr->nb_files; i++) {
        lxr_entry* entry = (lxr_entry*)&buf[sizeof(lxr_header) + i * sizeof(lxr_entry)];
        assert(entry->offset + entry->size <= lxr_size);
        // Ignore "dummy" entries
        if ((entry->size == 0) && (strcmp(entry->filename, "dummy") == 0))
            continue;
        snprintf(path, sizeof(path), "%s%c%s", argv[1], PATH_SEP, entry->filename);
        FILE* dst = fopen(path, "wb");
        if (dst == NULL) {
            fprintf(stderr, "ERROR: Can't create file '%s'\n", path);
            goto out;
        }
        if (fwrite(&buf[entry->offset], 1, entry->size, dst) != entry->size) {
            fprintf(stderr, "ERROR: Can't write file '%s'\n", path);
            fclose(dst);
            goto out;
        }
        printf("%08x %08x %s\n", entry->offset, entry->size, path);
        fclose(dst);
    }

    r = 0;

out:
    free(buf);
    fclose(src);
    return r;
}