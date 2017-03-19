/*
 * spooldir.h
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#ifndef SPOOLDIR_H
#define SPOOLDIR_H

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


enum spooldir_flags {
    SPOOLDIR_OPEN   = 1 << 0,
    SPOOLDIR_CREATE = 1 << 1,
} spooldir_flags;


/*
 * Represents a transaction. All the transactions have a pair of functions
 * which implement them: spooldir_<name>() creates the transaction and
 * populates a "spooltxn", and spooldir_<name>_commit() finalizes the
 * transaction.
 */
typedef struct {
    const spoolkey *key;
    int fd;
} spooltxn;


/*
 * Opens a spool directory, optionally creating it. The "mode" parameter
 * determines the permissions of the created spool directories and files
 * on disk, as in open(2).
 */
spooldir* spooldir_open (const char *path, enum spooldir_flags flags, int mode);

/*
 * Obtains the path where the spool directory resides on disk. The returned
 * string is owned by the spool directory and must *not* be freed.
 */
const char* spooldir_path (const spooldir *spool);

/*
 * Closes a spool directory, possibly freeing resources.
 */
void spooldir_close (spooldir *spool);

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
int spooldir_add_commit (spooldir *spool, spooltxn *txn);

/*
 */
int spooldir_open_new (spooldir *spool, const spoolkey *key);
int spooldir_open_cur (spooldir *spool, const spoolkey *key);

/*
 */
int spooldir_handle (spooldir *spool, const spoolkey *key, spooltxn *txn);
int spooldir_handle_commit (spooldir *spool, spooltxn *txn);

/*
 * Checks whether an element exists in the spool directory, but it has
 * not yet been fetched from it (i.e. it is "new").
 */
_Bool spooldir_is_new (const spooldir *spool, const spoolkey *key);

/*
 * Checks whether an element exists in the spool directory, and it has
 * already been fetched (i.e. it is not "new").
 */
_Bool spooldir_is_cur (const spooldir *spool, const spoolkey *key);

/*
 * These values are passed to callback functions, and they indicate what is
 * the status of the element being notified.
 */
enum spooldir_status {
    SPOOLDIR_STATUS_NEW,  /* A "new" element has been added. */
    SPOOLDIR_STATUS_CUR,  /* An element has been picked for handling. */
    SPOOLDIR_STATUS_FIN,  /* An element has left the spool directory. */
};

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
