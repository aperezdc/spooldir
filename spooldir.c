/*
 * spooldir.c
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "dbg.h"
#include "spooldir.h"
#include "hmac-sha256/hmac-sha256.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
# define HAVE_ARC4RANDOM 1
#else
#endif /* BSD */

struct _spoolkey {
    _Bool    inheap;
    uint16_t length;
    char    *bytes;
};

struct _spooldir {
    int tmp_fd;
    int cur_fd;
    int new_fd;
    const char path[];
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


static pthread_key_t rng_tls_key;
static pthread_once_t rng_once = PTHREAD_ONCE_INIT;

struct rng {
    uint8_t  key[RNG_KEY_SIZE];
    uint64_t count;
};

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

    /* Make the digest into an hex string. */
    for (size_t i = 0; i < HMAC_SHA256_DIGEST_SIZE; i++) {
        snprintf (key->bytes + i * 2, 3, "%02x", digest[i]);
    }
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
