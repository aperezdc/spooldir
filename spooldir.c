/*
 * spooldir.c
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "spooldir.h"
#include "dbg.h"
#include "hexify/hexify.h"
#include "hmac-sha256/hmac-sha256.h"
#include "mkdirp/mkdirp.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <stdbool.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#warning You system headers do not define O_CLOEXEC
#endif /* !O_CLOEXEC */

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#warning Your system headers do not define O_DIRECTORY
#endif /* !O_DIRECTORY */

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#warning Your system headers do not define O_NOFOLLOW
#endif /* !O_NOFOLLOW */

#ifndef O_PATH
#define O_PATH 0
#warning Your system headers do not define O_PATH
#endif /* !O_PATH */


enum {
    SPOOLDIR_DIR_O_FLAGS = O_DIRECTORY | O_NOFOLLOW | O_PATH,
    SPOOLDIR_FILE_O_FLAGS = O_NOFOLLOW,
};


#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
# define HAVE_ARC4RANDOM 1
# define HAVE_RENAMEAT   1
#elif defined(__linux) || defined(__linux__)
# define HAVE_RENAMEAT   1
# include <linux/fs.h>
# include <syscall.h>
# define renameat(ifd, iname, ofd, oname) \
    (syscall (SYS_renameat2, (ifd), (iname), (ofd), (oname), RENAME_NOREPLACE))
#endif


struct _spoolkey {
    _Bool    inheap;
    uint16_t length;
    char    *bytes;
};


struct _spooldir {
    int dir_fd;
    int tmp_fd;
    int new_fd;
    int wip_fd;
    int cur_fd;
};


#define RNG_KEY_SIZE HMAC_SHA256_DIGEST_SIZE


static void
random_bytes (void *buffer, size_t len)
{
    FILE *f;
    size_t pos = 0;

    if ((f = fopen ("/dev/urandom", "rb"))) {
        /* Assume that each fread() will return one byte at least */
        size_t tries = len;
        while (pos < len && tries--) {
            pos += fread (buffer, 1, len, f);
        }
        fclose (f);
        if (pos >= len) {
            return;
        }
    }

    /* Use the fall-back mechanism to read (len-pos) bytes. */
    assert_ok (pos < len);

#if HAVE_ARC4RANDOM
    arc4random_buf (((uint8_t*) buffer) + pos, len - pos);
#else
    /* TODO: Try using getrandom() on GNU/Linux, or rand48(). */
    while (pos < len) ((uint8_t*) buffer)[pos++] = rand ();
#endif
}


struct rng {
    uint8_t  key[RNG_KEY_SIZE];
    uint64_t count;
};

static pthread_key_t rng_tls_key;
static pthread_once_t rng_once = PTHREAD_ONCE_INIT;


static void
init_rng_tls_key (void)
{
    (void) pthread_key_create (&rng_tls_key, NULL);
    srand ((unsigned int) (getpid () ^ time (NULL)));
}


static struct rng*
get_rng (void)
{
    (void) pthread_once (&rng_once, init_rng_tls_key);

    struct rng *r;
    if ((r = pthread_getspecific (rng_tls_key)) == NULL) {
        r = (struct rng*) calloc (1, sizeof(struct rng));
        random_bytes (r->key, RNG_KEY_SIZE);
        (void) pthread_setspecific (rng_tls_key, r);
    }
    assert_not_null (r);
    return r;
}


/*
 * Generates a new unique key (filename) to be used for a spool element. The original
 * algorithm is described in the Maildir specification by Daniel J. Bernstein:
 * http://cr.yp.to/proto/maildir.html
 *
 * Unfortunately, the original algorithm leaks some host specific information, which
 * would be nicer to avoid. Instead, this uses HMAC-SHA256 with a random seed as key,
 * and the bytes of en ever-increasing counter as data payload. This approach is used
 * in Salvatore “antirez” Sanfillippo's Disque: http://antirez.com/news/99
 */
spoolkey*
spoolkey_new (void)
{
    uint8_t digest[HMAC_SHA256_DIGEST_SIZE];

    /* Generate HMAC-SHA256(K, count) */
    struct rng *rng = get_rng ();
    hmac_sha256 (digest,
                 (const uint8_t*) &rng->count, sizeof (rng->count),
                 rng->key, RNG_KEY_SIZE);
    rng->count++;

    spoolkey *key = (spoolkey*) calloc (1, sizeof(spoolkey) + RNG_KEY_SIZE * 2 + 1);
    key->inheap = false;
    key->length = RNG_KEY_SIZE * 2;
    key->bytes = (char*) &key[1];
    int nbytes = hexify (digest, HMAC_SHA256_DIGEST_SIZE, key->bytes, key->length + 1);
    assert_equal (nbytes, key->length);
    (void) nbytes;
    return key;
}


spoolkey*
spoolkey_new_from_mem (const void *bytes, size_t len, _Bool copy, _Bool take_ownership)
{
    api_check_return_val (bytes, NULL);
    api_check_return_val (len > 0, NULL);

    size_t extra_size = copy ? len + 1 : 0;
    spoolkey *key = (spoolkey*) calloc (1, sizeof(spoolkey) + extra_size);
    key->inheap = take_ownership && !copy;
    key->length = len;
    key->bytes = (char*) (copy
        ? memcpy(&key[1] /* Point right after the struct. */, bytes, len)
        : bytes);
    return key;
}


spoolkey*
spoolkey_copy (const spoolkey *key)
{
    api_check_return_val (key, NULL);
    return spoolkey_new_from_mem (key->bytes, key->length, true, true);
}


const char*
spoolkey_cstr (const spoolkey *key)
{
    api_check_return_val (key, NULL);
    return key->bytes;
}


void
spoolkey_free (spoolkey *key)
{
    api_check_return (key);
    if (key->inheap) free (key->bytes);
    free (key);
}


spoolkey*
spooltxn_take_key (spooltxn *txn)
{
    api_check_return_val (txn, NULL);

    spoolkey *key = txn->key;
    txn->key = NULL;
    return key;
}


int
spooltxn_take_fd (spooltxn *txn)
{
    api_check_return_val (txn, -1);

    int fd = txn->fd;
    txn->fd = -1;
    return fd;
}


static int
open_or_create_subdir (int dir_fd, const char *subdir)
{
    struct stat sb;
    if (fstatat (dir_fd, subdir, &sb, AT_SYMLINK_NOFOLLOW)) {
        if (errno == ENOENT) {
            /* Create. */
            if (mkdirat (dir_fd, subdir, S_IRWXU) && errno != EEXIST) {
                return -1;
            }
        } else {
            return -1;
        }
    }

    return openat (dir_fd, subdir, O_RDWR | SPOOLDIR_DIR_O_FLAGS);
}


spooldir*
spooldir_open_path (const char *path, uint32_t mode)
{
    api_check_return_val (path, NULL);

    int dir_fd = open (path, O_RDWR | SPOOLDIR_DIR_O_FLAGS, 0);
    if (dir_fd < 0) {
        if (mode && errno == ENOENT) {
            if ((mkdirp (path, (mode_t) mode) == -1) ||
                (dir_fd = open (path, O_RDWR | SPOOLDIR_DIR_O_FLAGS, 0)) == -1)
            {
                return NULL;
            }
        } else {
            return NULL;
        }
    }

    spooldir *spool = spooldir_open (dir_fd);
    if (!spool) close (dir_fd);
    return spool;
}


/*
 * TODO: This should flock() the top-level directory to ensure that two
 * different processes do not try to mkdir() the subdirectories at the
 * same time!
 */
spooldir* spooldir_open (int dir_fd)
{
    api_check_return_val (dir_fd >= 0, NULL);

    /* Check for existence, create if needed (and requested). */
    struct stat sb;
    if (fstat (dir_fd, &sb)) {
        /* TODO: Report errors. */
        return NULL;
    }
    if (!S_ISDIR (sb.st_mode)) {
        /* TODO: Report errors. */
        return NULL;
    }

    int tmp_fd = -1, new_fd = -1, wip_fd = -1, cur_fd = -1;
    if ((tmp_fd = open_or_create_subdir (dir_fd, "tmp")) < 0 ||
        (new_fd = open_or_create_subdir (dir_fd, "new")) < 0 ||
        (wip_fd = open_or_create_subdir (dir_fd, "wip")) < 0 ||
        (cur_fd = open_or_create_subdir (dir_fd, "cur")) < 0)
    {
        /* TODO: Report errors. */
        goto close_and_cleanup;
    }

    spooldir *spool = (spooldir*) calloc (1, sizeof (spooldir));
    spool->dir_fd = dir_fd;
    spool->tmp_fd = tmp_fd;
    spool->new_fd = new_fd;
    spool->wip_fd = wip_fd;
    spool->cur_fd = cur_fd;
    return spool;

close_and_cleanup:
    if (tmp_fd >= 0) close (tmp_fd);
    if (new_fd >= 0) close (new_fd);
    if (wip_fd >= 0) close (wip_fd);
    if (cur_fd >= 0) close (cur_fd);
    return NULL;
}


void
spooldir_close (spooldir *spool)
{
    api_check_return (spool);

    close (spool->tmp_fd);
    close (spool->new_fd);
    close (spool->wip_fd);
    close (spool->cur_fd);

    free (spool);
}


static inline int
status_to_fd (const spooldir *spool, enum spooldir_status status)
{
    assert_not_null (spool);
    switch (status) {
        case SPOOLDIR_STATUS_TMP: return spool->tmp_fd;
        case SPOOLDIR_STATUS_WIP: return spool->wip_fd;
        case SPOOLDIR_STATUS_NEW: return spool->new_fd;
        case SPOOLDIR_STATUS_CUR: return spool->cur_fd;
        default: return -1;
    }
}


/*
 * We cannot directly rename() files, because that could overwrite existing
 * files at the destination directory. Instead, prefer to use link(), which
 * behaves atomically:
 *
 *    1. Link the file under the destination directory.
 *    2. Open the file at the destination directory. On failure:
 *         2.1. Unlink from the destination directory.
 *    3. Unlink from the source directory. On failure:
 *         3.1. Unlink from the destination directory.
 *         3.2. Close the open file descriptor.
 */
static inline int
relink_and_open (int src_fd, int dst_fd, const char *name)
{
    assert_ok (src_fd >= 0);
    assert_ok (dst_fd >= 0);
    assert_ok (name != NULL);

    int retval = -1;
    int fd = -1;

    if ((retval = linkat (src_fd, name, dst_fd, name, 0)) < 0) {
        retval = -errno;
        goto error;
    }
    if ((fd = openat (dst_fd, name, O_RDWR | SPOOLDIR_FILE_O_FLAGS, 0)) < 0) {
        retval = -errno;
        goto error_unlink_dst;
    }
    if ((retval = unlinkat (src_fd, name, 0)) <  0) {
        retval = -errno;
        goto error_unlink_dst_close_fd;
    }
    return fd;

error_unlink_dst_close_fd:
    assert_ok (fd >= 0);
    close (fd);
error_unlink_dst:
    unlinkat (dst_fd, name, 0);
error:
    return retval;
}


int
spooldir__open_file (spooldir *spool, const spoolkey *key,
                     enum spooldir_status status, int oflag)
{
    api_check_return_val (spool, -1);
    api_check_return_val (key, -1);
    api_check_return_val (status != SPOOLDIR_STATUS_FIN, -1);

    int subdir_fd = status_to_fd (spool, status);
    if (subdir_fd < 0)
        return -1;

    return openat (subdir_fd, key->bytes, oflag | SPOOLDIR_FILE_O_FLAGS);
}


int
spooldir_add (spooldir *spool, spooltxn *txn)
{
    api_check_return_val (spool, -1);
    api_check_return_val (txn, -1);

    txn->key = spoolkey_new ();
    txn->fd = openat (spool->tmp_fd, txn->key->bytes,
                      O_CREAT | O_EXCL | O_RDWR | SPOOLDIR_FILE_O_FLAGS, 0666);

    if (txn->fd < 0) {
        spoolkey_free (txn->key);
        txn->key = NULL;
    } else {
        txn->status = SPOOLDIR_STATUS_TMP;
    }

    return txn->fd;
}


int
spooldir_commit (spooldir *spool, spooltxn *txn)
{
    api_check_return_val (spool, -1);
    api_check_return_val (txn, -1);

    int retval = -1;

    switch (txn->status) {
        case SPOOLDIR_STATUS_TMP:
            txn->status = SPOOLDIR_STATUS_NEW;
            retval = renameat (spool->tmp_fd, txn->key->bytes,
                               spool->new_fd, txn->key->bytes);
            break;

        case SPOOLDIR_STATUS_WIP:
            txn->status = SPOOLDIR_STATUS_CUR;
            retval = renameat (spool->wip_fd, txn->key->bytes,
                               spool->cur_fd, txn->key->bytes);
            break;

        case SPOOLDIR_STATUS_CUR:
        case SPOOLDIR_STATUS_NEW:
        case SPOOLDIR_STATUS_FIN:
            api_check_return_val (false, -1);
            errno = EINVAL;
            return -1;
    }

    if (txn->key) {
        spoolkey_free (txn->key);
        txn->key = NULL;
    }
    if (txn->fd >= 0) {
        close (txn->fd);
        txn->fd = -1;
    }
    return retval;
}


int
spooldir_rollback (spooldir *spool, spooltxn *txn)
{
    api_check_return_val (spool, -1);
    api_check_return_val (txn, -1);

    int retval = -1;

    switch (txn->status) {
        case SPOOLDIR_STATUS_TMP:
            retval = unlinkat (spool->tmp_fd, txn->key->bytes, 0);
            break;

        case SPOOLDIR_STATUS_WIP:
            txn->status = SPOOLDIR_STATUS_NEW;
            retval = renameat (spool->wip_fd, txn->key->bytes,
                               spool->new_fd, txn->key->bytes);
            break;

        case SPOOLDIR_STATUS_NEW:
        case SPOOLDIR_STATUS_CUR:
        case SPOOLDIR_STATUS_FIN:
            api_check_return_val (false, -1);
            errno = EINVAL;
            return -1;
    }

    if (txn->key) {
        spoolkey_free (txn->key);
        txn->key = NULL;
    }
    if (txn->fd >= 0) {
        close (txn->fd);
        txn->fd = -1;
    }
    return retval;
}


struct pick_txn {
    spooltxn txn_base;
    DIR     *dirp;
};


int
spooldir_pick (spooldir *spool, spooltxn *txn)
{
    api_check_return_val (spool, -1);
    api_check_return_val (spool->new_fd >= 0, -1);
    api_check_return_val (spool->wip_fd >= 0, -1);
    api_check_return_val (txn, -1);

    struct pick_txn *ptxn = (struct pick_txn*) txn;

    int dir_fd = dup (spool->new_fd);
    if (dir_fd < 0)
        return dir_fd;

    if (!(ptxn->dirp = fdopendir (dir_fd)))
        return -1;

    struct dirent *de = NULL;
    int retval = -1;

    for (;;) {
        errno = 0;
        if (!(de = readdir (ptxn->dirp))) {
            if (!errno) retval = EOF;
            goto cleanup;
        }

        if (de->d_name[0] == '.')  /* Skip hidden files */
            continue;

        /* Use fstatat() as fall-back to fill the field. */
        if (de->d_type == DT_UNKNOWN) {
            struct stat sb;
            if (fstatat (spool->new_fd, de->d_name, &sb, AT_SYMLINK_NOFOLLOW) < 0)
                goto cleanup;
            if (S_ISREG (sb.st_mode))  /* We are only interested in regular files */
                de->d_type = DT_REG;
        }

        if (de->d_type == DT_REG)
            break;
    }

    if ((txn->fd = relink_and_open (spool->new_fd, spool->wip_fd, de->d_name)) < 0)
        goto cleanup;

    txn->key = spoolkey_new_from_string (de->d_name, true, true);
    txn->status = SPOOLDIR_STATUS_WIP;
    return 0;

cleanup:
    {
        int saved_errno = errno;  /* Can be changed by closedir. */
        if (ptxn->dirp) {
            closedir (ptxn->dirp);
            ptxn->dirp = NULL;
        }
        errno = saved_errno;
    }
    return retval;
}


_Bool
spooldir_has_status (const spooldir *spool, const spoolkey *key,
                     enum spooldir_status status)
{
    api_check_return_val (spool, false);
    api_check_return_val (key, false);

    int subdir_fd = status_to_fd (spool, status);
    if (subdir_fd < 0)
        return false;

    struct stat sb;
    return (fstatat (subdir_fd, key->bytes, &sb, AT_SYMLINK_NOFOLLOW) == 0)
        && (S_ISREG (sb.st_mode));
}
