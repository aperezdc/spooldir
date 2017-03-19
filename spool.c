/*
 * spool.c
 * Copyright (C) 2017 Adrian Perez <aperez@igalia.com>
 *
 * Distributed under terms of the MIT license.
 */

#include "spooldir.h"
#include <stdlib.h>
#include <stdio.h>


int
main (int argc, char *argv[])
{
    spoolkey *key = spoolkey_new ();
    printf ("%s\n", spoolkey_cstr (key));
    spoolkey_free (key);
    return EXIT_SUCCESS;
}
