/*
 * spool.c
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "spooldir.h"
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>


static int
help_exit (int code, const char *argv0)
{
    fprintf (stderr, "Usage: %s <spooldir> [path]\n", argv0);
    exit (code);
    return code;
}


static int
err_exit (int e, const char *fmt, ...)
{
    va_list args;
    va_start (args, fmt);
    vfprintf (stderr, fmt, args);
    va_end (args);
    fprintf (stderr, " (reason: %s)\n", strerror (e));
    fflush (stderr);
    exit (EXIT_FAILURE);
}


enum {
    COPY_BUFSZ = 4096,
};

static _Bool
copy_contents (FILE *i, FILE *o)
{
    uint8_t buffer[COPY_BUFSZ];
    size_t count;

    while ((count = fread (buffer, 1, COPY_BUFSZ, i)) > 0 && !feof (i)) {
        if (ferror (i))
            return false;
        if (fwrite (buffer, 1, count, o) < count && ferror (o))
            return false;
    }

    return true;
}


int
main (int argc, char *argv[])
{
    if (argc != 2 && argc != 3)
        return help_exit (EXIT_FAILURE, argv[0]);
    if (strcmp (argv[1], "--help") == 0 || strcmp (argv[1], "-h") == 0)
        return help_exit (EXIT_SUCCESS, argv[0]);

    FILE *fi = (argc == 3) ? fopen (argv[2], "rb") : stdin;
    if (!fi)
        return err_exit (errno, "Could not open '%s' for reading", argv[2]);

    spooldir *spool = spooldir_open_path (argv[1], 0777);
    if (!spool) return err_exit (errno, "Could not open spool '%s'", argv[1]);

    spooltxn txn;
    if (spooldir_add (spool, &txn) < 0) {
        spooldir_close (spool);
        return err_exit (errno, "Could not add item to spool");
    }

    /* Put some content in the newwly created spool element. */
    FILE *fo = fdopen (spooltxn_take_fd (&txn), "w");
    if (!(fo && copy_contents (fi, fo))) {
        spooldir_rollback (spool, &txn);
        spooldir_close (spool);
        return EXIT_FAILURE;
    }
    if (fo)
        fclose (fo);

    /* Keep a copy around to be able to print the resulting filename. */
    spoolkey *key = spoolkey_copy (txn.key);

    if (spooldir_commit (spool, &txn) < 0) {
        int e = errno;
        spooldir_close (spool);
        return err_exit (e, "Coult not commit item to spool");
    }
    spooldir_close (spool);

    printf ("%s\n", spoolkey_cstr (key));
    spoolkey_free (key);

    return EXIT_SUCCESS;
}
