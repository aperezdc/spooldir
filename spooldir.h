/*
 * spooldir.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef SPOOLDIR_H
#define SPOOLDIR_H

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE 1

#include <stdint.h>
#include <string.h>

typedef struct _spooldir spooldir;
typedef struct _spoolkey spoolkey;

/*
 */
spoolkey* spoolkey_new (void);

/*
 * Creates a new key given a new key from a C string.
 */
spoolkey* spoolkey_new_from_mem (const void *bytes, size_t slen, _Bool copy, _Bool take_ownwership);

static inline spoolkey*
spoolkey_new_from_string (const char *str, _Bool copy, _Bool take_ownership)
{
    return spoolkey_new_from_mem (str, strlen (str), copy, take_ownership);
}

/*
 * Creates a copy of a key.
 */
spoolkey* spoolkey_copy (const spoolkey *key);

/*
 * Obtains the C string representation of a key.
 */
const char* spoolkey_cstr (const spoolkey *key);

/*
 * Frees memory used by a key.
 */
void spoolkey_free (spoolkey *key);


/*
 * Status of an element in a spool dir.
 */
enum spooldir_status {
    SPOOLDIR_STATUS_TMP,  /* An element is in an undefined state. */
    SPOOLDIR_STATUS_NEW,  /* An element is new and just added. */
    SPOOLDIR_STATUS_WIP,  /* An element is being inspected for handling. */
    SPOOLDIR_STATUS_CUR,  /* An element has been picked for handling. */
    SPOOLDIR_STATUS_FIN,  /* An element has left the spool directory. */
};

/*
 * Represents a transaction. All the transactions have a pair of functions
 * which implement them: spooldir_<name>() creates the transaction and
 * populates a "spooltxn", and spooldir_<name>_commit() finalizes the
 * transaction.
 */
enum { SPOOLTXN__PAD = 4 * sizeof (uintptr_t) };

typedef struct {
    enum spooldir_status status;
    spoolkey            *key;
    int                  fd;
    uint8_t              __pad[SPOOLTXN__PAD];  /* Private. */
} spooltxn;

spoolkey* spooltxn_take_key (spooltxn *txn);
int spooltxn_take_fd (spooltxn *txn);

/*
 * Opens a spool directory given an open file descriptor to a directory.
 */
spooldir* spooldir_open (int dir_fd);

/*
 */
spooldir* spooldir_open_path (const char *path, uint32_t mode);

/*
 * Closes a spool directory, possibly freeing resources.
 */
void spooldir_close (spooldir *spool);

/*
 * Low-level interface to open files under the spoold directory.
 */
int spooldir__open_file (spooldir *spool, const spoolkey *key,
                         enum spooldir_status status, int oflag);

/*
 */
int spooldir_commit (spooldir *spool, spooltxn *txn);
int spooldir_rollback (spooldir *spool, spooltxn *txn);

/*
 * Starts the creation of a new element in the spool directory.
 *
 * On success, "0" is returned and the "txn" transaction is populated with a
 * valid file descriptor opened in "O_RDWR" mode, and the key that will be
 * used for the new element. Once the file contents have been written, the
 * element still needs to be commited using "spooldir_add_commit()".
 *
 * In case of failure, a non-zero integer with the "errno" that caused the
 * failure is returned.
 */
int spooldir_add (spooldir *spool, spooltxn *txn);

/*
 */
int spooldir_pick (spooldir *spool, spooltxn *txn);

/*
 */
int spooldir_delete (spooldir *spool, const spoolkey *key);

/*
 */
_Bool spooldir_has_status (const spooldir *spool, const spoolkey *key,
                           enum spooldir_status status);

/*
 * Type of callback functions used for notifying of element status. The
 * transaction "txn" argument always has a valid key associated, the file
 * descriptor will be initially invalid.
 */
typedef void (*spooldircfn) (spooldir*, enum spooldir_status, spooltxn *txn, void *userdata);

/*
 * Listens for updates in a spool directory, and invokes the "callback"
 * function whenever an item changes their status.
 */
int spooldir_notify (spooldir *spool, spooldircfn callback, void *userdata);

#endif /* !SPOOLDIR_H */
