/*
 * Copyright 2014 LKC Technologies, Inc.
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

#include "fwup_create.h"
#include "cfgfile.h"
#include "util.h"
#include "fwfile.h"
#include "sparse_file.h"
#include "../config.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <archive.h>
#include <archive_entry.h>
#include <sodium.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

struct calc_metadata_state
{
    struct sparse_file_map sfm;
    struct sparse_file_read_iterator read_iterator;

    crypto_generichash_state hash_state;
};

static int build_sparse_map(int fd, void *cookie)
{
    struct calc_metadata_state *state = (struct calc_metadata_state *) cookie;
    return sparse_file_build_map_from_fd(fd, &state->sfm);
}

static int calc_hash(int fd, void *cookie)
{
    struct calc_metadata_state *state = (struct calc_metadata_state *) cookie;

    off_t offset = 0;
    for (;;) {
        char buffer[4096];
        size_t len;
        OK_OR_RETURN(sparse_file_read_next_data(&state->read_iterator, fd, &offset, buffer, sizeof(buffer), &len));
        if (len == 0)
            break;

        crypto_generichash_update(&state->hash_state, (unsigned char*) buffer, len);
    }
    return 0;
}

struct write_file_state
{
    struct archive *a;
    struct sparse_file_read_iterator read_iterator;
};

static int write_file_to_archive(int fd, void *cookie)
{
    struct write_file_state *state = (struct write_file_state *) cookie;

    off_t offset = 0;
    for (;;) {
        char buffer[4096];
        size_t len;
        OK_OR_RETURN(sparse_file_read_next_data(&state->read_iterator, fd, &offset, buffer, sizeof(buffer), &len));
        if (len == 0)
            break;

        off_t written = archive_write_data(state->a, buffer, len);
        if (written != (off_t) len)
            ERR_RETURN("error writing to archive");
    }
    return 0;
}

static int run_on_each_path(const char *file_resource, const char *paths, int (*func)(int, void*), void *cookie)
{
    int rc = 0;
    char *paths_copy = strdup(paths);
    for (char *path = strtok(paths_copy, ";");
         path != NULL && rc == 0;
         path = strtok(NULL, ";")) {
        int fd = open(path, O_RDONLY | O_WIN32_BINARY);
        if (fd < 0) {
            set_last_error("can't open path '%s' in file-resource '%s'", path, file_resource);
            free(paths_copy);
            return -1;
        }
        rc = func(fd, cookie);
        close(fd);
    }
    free(paths_copy);
    return rc;
}

static int compute_file_metadata(cfg_t *cfg)
{
    cfg_t *sec;
    int i = 0;

    while ((sec = cfg_getnsec(cfg, "file-resource", i++)) != NULL) {
        const char *paths = cfg_getstr(sec, "host-path");
        if (!paths)
            ERR_RETURN("host-path must be set for file-resource '%s'", cfg_title(sec));

        struct calc_metadata_state state;

        // Compute the sparse file map
        sparse_file_init(&state.sfm);
        OK_OR_RETURN(run_on_each_path(cfg_title(sec), paths, build_sparse_map, &state));
        OK_OR_RETURN(sparse_file_set_map_in_resource(sec, &state.sfm));

        // Compute the hash across the files
        crypto_generichash_init(&state.hash_state, NULL, 0, crypto_generichash_BYTES);
        sparse_file_start_read(&state.sfm, &state.read_iterator);
        OK_OR_RETURN(run_on_each_path(cfg_title(sec), paths, calc_hash, &state));

        unsigned char hash[crypto_generichash_BYTES];
        crypto_generichash_final(&state.hash_state, hash, sizeof(hash));
        char hash_str[sizeof(hash) * 2 + 1];
        bytes_to_hex(hash, hash_str, sizeof(hash));

        cfg_setstr(sec, "blake2b-256", hash_str);

        sparse_file_free(&state.sfm);
    }

    return 0;
}

static int add_file_resource(struct archive *a,
                          const char *resource_name,
                          const char *local_paths,
                          const struct sparse_file_map *sfm,
                          const struct fwfile_assertions *assertions)
{
    int rc = 0;
    struct archive_entry *entry = archive_entry_new();

    if (*local_paths == '\0')
        ERR_CLEANUP_MSG("must specify a host-path for resource '%s'", resource_name);

    off_t total_len = sparse_file_size(sfm);

    if (assertions) {
        if (assertions->assert_gte >= 0 &&
                !(total_len >= assertions->assert_gte))
            ERR_CLEANUP_MSG("file size assertion failed on '%s'. Size is %d bytes. It must be >= %d bytes (%d blocks)",
                            local_paths, total_len, assertions->assert_gte, assertions->assert_gte / 512);
        if (assertions->assert_lte >= 0 &&
                !(total_len <= assertions->assert_lte))
            ERR_CLEANUP_MSG("file size assertion failed on '%s'. Size is %d bytes. It must be <= %d bytes (%d blocks)",
                            local_paths, total_len, assertions->assert_lte, assertions->assert_lte / 512);
    }

    // Convert the resource name to an archive path (most resources should be in the data directory)
    char archive_path[FWFILE_MAX_ARCHIVE_PATH];
    size_t resource_name_len = strlen(resource_name);
    if (resource_name_len + 6 > sizeof(archive_path))
        ERR_CLEANUP_MSG("resource name '%s' is too long", resource_name);
    if (resource_name_len == '\0')
        ERR_CLEANUP_MSG("resource name can't be empty");
    if (resource_name[resource_name_len - 1] == '/')
        ERR_CLEANUP_MSG("resource name '%s' can't end in a '/'", resource_name);

    if (resource_name[0] == '/') {
        if (resource_name[1] == '\0')
            ERR_CLEANUP_MSG("resource name can't be the root directory");

        // This seems like it's just asking for trouble, so error out.
        if (strcmp(resource_name, "/meta.conf") == 0)
            ERR_CLEANUP_MSG("resources can't be named /meta.conf");

        // Absolute paths are not intended to be commonly used and ones
        // in /data won't work when applying the updates, so error out.
        if (memcmp(resource_name, "/data/", 6) == 0 ||
            strcmp(resource_name, "/data") == 0)
            ERR_CLEANUP_MSG("use a normal resource name rather than specifying /data");

        strcpy(archive_path, &resource_name[1]);
    } else {
        sprintf(archive_path, "data/%s", resource_name);
    }

    off_t data_len = sparse_file_data_size(sfm);

    archive_entry_set_pathname(entry, archive_path);
    archive_entry_set_size(entry, data_len);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_write_header(a, entry);

    struct write_file_state state;
    state.a = a;
    sparse_file_start_read(sfm, &state.read_iterator);
    OK_OR_CLEANUP(run_on_each_path(resource_name, local_paths, write_file_to_archive, &state));

cleanup:
    archive_entry_free(entry);
    return rc;
}

static int add_file_resources(cfg_t *cfg, struct archive *a)
{
    cfg_t *sec;
    int i = 0;
    int rc = 0;

    struct sparse_file_map sfm;
    sparse_file_init(&sfm);

    while ((sec = cfg_getnsec(cfg, "file-resource", i++)) != NULL) {
        const char *hostpath = cfg_getstr(sec, "host-path");
        if (!hostpath)
            ERR_CLEANUP_MSG("specify a host-path");

        struct fwfile_assertions assertions;
        assertions.assert_lte = cfg_getint(sec, "assert-size-lte") * 512;
        assertions.assert_gte = cfg_getint(sec, "assert-size-gte") * 512;

        OK_OR_CLEANUP(sparse_file_get_map_from_resource(sec, &sfm));

        OK_OR_CLEANUP(add_file_resource(a, cfg_title(sec), hostpath, &sfm, &assertions));
    }

cleanup:
    sparse_file_free(&sfm);
    return rc;
}

static int create_archive(cfg_t *cfg, const char *filename, const unsigned char *signing_key)
{
    int rc = 0;
    struct archive *a = archive_write_new();
    if (archive_write_set_format_zip(a) != ARCHIVE_OK ||
        archive_write_zip_set_compression_deflate(a) != ARCHIVE_OK)
        ERR_CLEANUP_MSG("error configuring libarchive: %s", archive_error_string(a));

    // Setting the compression-level is only supported on more recent versions
    // of libarchive, so don't check for errors.
    archive_write_set_format_option(a, "zip", "compression-level", "9");

    if (archive_write_open_filename(a, filename) != ARCHIVE_OK)
        ERR_CLEANUP_MSG("error creating archive '%s': %s", filename, archive_error_string(a));

    OK_OR_CLEANUP(fwfile_add_meta_conf(cfg, a, signing_key));

    OK_OR_CLEANUP(add_file_resources(cfg, a));

cleanup:
    archive_write_close(a);
    archive_write_free(a);

    return rc;
}

int fwup_create(const char *configfile, const char *output_firmware, const unsigned char *signing_key)
{
    cfg_t *cfg = NULL;
    int rc = 0;

    // Parse configuration
    OK_OR_CLEANUP(cfgfile_parse_file(configfile, &cfg));

    // Automatically add fwup metadata
    cfg_setstr(cfg, "meta-creation-date", get_creation_timestamp());
    cfg_setstr(cfg, "meta-fwup-version", PACKAGE_VERSION);

    // Compute all metadata
    OK_OR_CLEANUP(compute_file_metadata(cfg));

    // Create the archive
    OK_OR_CLEANUP(create_archive(cfg, output_firmware, signing_key));

cleanup:
    if (cfg)
        cfgfile_free(cfg);

    return rc;
}
