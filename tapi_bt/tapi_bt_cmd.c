/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Bluetooth TAPI: running a tool, and reading what it printed
 *
 * The three systems answer in three shapes - lines of text from BlueZ,
 * JSON from blueutil, JSON from PowerShell - so the parsing helpers
 * live here next to the runner rather than in whichever module needed
 * one first.
 */

#define TE_LGR_USER "TAPI BT"

#include "te_config.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "logger_api.h"
#include "te_str.h"
#include "te_string.h"
#include "te_vector.h"

#include "tapi_bt_internal.h"

/** Arguments of a command, as a plain vector of strings. */
typedef struct bt_cmd_opt {
    /** Number of arguments. */
    size_t n_args;
    /** Arguments after argv[0]. */
    const char **args;
} bt_cmd_opt;

static const tapi_job_opt_bind bt_cmd_binds[] = TAPI_JOB_OPT_SET(
    TAPI_JOB_OPT_ARRAY_PTR(bt_cmd_opt, n_args, args,
        TAPI_JOB_OPT_CONTENT(TAPI_JOB_OPT_STRING, NULL, false))
);

/* See description in tapi_bt_internal.h */
void
tapi_bt_arg(te_vec *args, const char *fmt, ...)
{
    te_string built = TE_STRING_INIT;
    char *arg;
    va_list ap;

    va_start(ap, fmt);
    te_string_append_va(&built, fmt, ap);
    va_end(ap);

    arg = built.ptr;
    TE_VEC_APPEND(args, arg);
}

/* See description in tapi_bt_internal.h */
te_errno
tapi_bt_cmd(tapi_job_factory_t *factory, const char *name,
            const char *program, const te_vec *args, int timeout_ms,
            te_string *out, te_string *err, int *exit_code)
{
    bt_cmd_opt opt = {
        .n_args = te_vec_size(args),
        .args = te_vec_size(args) == 0 ? NULL :
                (const char **)te_vec_get((te_vec *)args, 0),
    };
    tapi_devtool_output output;
    tapi_devtool_run run = TAPI_DEVTOOL_RUN_INIT;
    te_errno rc;

    rc = tapi_devtool_run_init(&run, factory, name, program, bt_cmd_binds,
                               &opt, NULL);
    if (rc != 0)
        return rc;

    rc = tapi_devtool_run_start(&run);
    if (rc == 0)
        rc = tapi_devtool_run_wait(&run, timeout_ms);

    if (rc != 0)
    {
        tapi_devtool_run_fini(&run);
        return rc;
    }

    tapi_devtool_run_get_output(&run, &output);

    if (out != NULL && output.out != NULL)
        te_string_append(out, "%s", output.out);
    if (err != NULL && output.err != NULL)
        te_string_append(err, "%s", output.err);

    if (exit_code != NULL)
    {
        /*
         * -1 for a signal, and it is not a corner case here:
         * bluetoothctl aborts out of libdbus when bluetoothd is not
         * running. Measured on BlueZ 5.72, that arrives as SIGABRT.
         * A caller that read it as a status would see 134, or 0, and
         * either way would believe an answer that does not exist.
         */
        *exit_code = output.status.type == TAPI_JOB_STATUS_EXITED ?
                     output.status.value : -1;
    }

    return tapi_devtool_run_fini(&run);
}

/* See description in tapi_bt_internal.h */
te_errno
tapi_bt_spawn(tapi_job_factory_t *factory, const char *name,
              const char *program, const te_vec *args,
              tapi_devtool_run *run)
{
    bt_cmd_opt opt = {
        .n_args = te_vec_size(args),
        .args = te_vec_size(args) == 0 ? NULL :
                (const char **)te_vec_get((te_vec *)args, 0),
    };
    te_errno rc;

    *run = (tapi_devtool_run)TAPI_DEVTOOL_RUN_INIT;

    rc = tapi_devtool_run_init(run, factory, name, program, bt_cmd_binds,
                               &opt, NULL);
    if (rc != 0)
        return rc;

    /* Started and left running: the caller waits for it to be ready. */
    rc = tapi_devtool_run_start(run);
    if (rc != 0)
        tapi_devtool_run_fini(run);

    return rc;
}

/* See description in tapi_bt_internal.h */
te_errno
tapi_bt_powershell(tapi_job_factory_t *factory, const char *script,
                   int timeout_ms, te_string *out, int *exit_code)
{
    te_vec args = TE_VEC_INIT(char *);
    te_string err = TE_STRING_INIT;
    te_errno rc;

    /*
     * -NoProfile so that whatever the machine's profile does cannot
     * reach the output, and -NonInteractive so a prompt fails rather
     * than waits for a person who is not there.
     */
    tapi_bt_arg(&args, "-NoProfile");
    tapi_bt_arg(&args, "-NonInteractive");
    tapi_bt_arg(&args, "-Command");
    tapi_bt_arg(&args, "%s", script);

    rc = tapi_bt_cmd(factory, "powershell", "powershell", &args, timeout_ms,
                     out, &err, exit_code);

    if (rc == 0 && err.len != 0)
        RING("PowerShell wrote to stderr: %s", te_string_value(&err));

    te_vec_deep_free(&args);
    te_string_free(&err);

    return rc;
}

/**
 * Find @c "key" in a flat JSON object and return what follows the
 * colon.
 */
static const char *
bt_json_value(const char *object, const char *key)
{
    te_string quoted = TE_STRING_INIT;
    const char *found;

    te_string_append(&quoted, "\"%s\"", key);
    found = strstr(object, quoted.ptr);
    te_string_free(&quoted);

    if (found == NULL)
        return NULL;

    found = strchr(found, ':');
    if (found == NULL)
        return NULL;

    found++;
    while (*found == ' ' || *found == '\t' || *found == '\n')
        found++;

    return found;
}

/* See description in tapi_bt_internal.h */
bool
tapi_bt_json_str(const char *object, const char *key, te_string *dest)
{
    const char *value = bt_json_value(object, key);
    const char *end;

    if (value == NULL || *value != '"')
        return false;

    value++;
    /*
     * No escape handling: the fields read here are addresses, device
     * names and identifiers. A name can hold a quote, and one that
     * does comes back truncated rather than wrong - which is visible
     * in the log, where a silently mangled name would not be.
     */
    end = strchr(value, '"');
    if (end == NULL)
        return false;

    te_string_append(dest, "%.*s", (int)(end - value), value);

    return true;
}

/* See description in tapi_bt_internal.h */
bool
tapi_bt_json_bool(const char *object, const char *key, bool *value)
{
    const char *found = bt_json_value(object, key);

    if (found == NULL)
        return false;

    if (strncmp(found, "true", 4) == 0)
        *value = true;
    else if (strncmp(found, "false", 5) == 0)
        *value = false;
    else
        return false;

    return true;
}

/* See description in tapi_bt_internal.h */
bool
tapi_bt_json_int(const char *object, const char *key, int *value)
{
    const char *found = bt_json_value(object, key);
    bool negative = false;
    int result = 0;
    bool any = false;

    if (found == NULL)
        return false;

    if (*found == '-')
    {
        negative = true;
        found++;
    }

    for (; *found >= '0' && *found <= '9'; found++)
    {
        result = result * 10 + (*found - '0');
        any = true;
    }

    if (!any)
        return false;

    *value = negative ? -result : result;

    return true;
}

/* See description in tapi_bt_internal.h */
const char *
tapi_bt_json_next(const char *array, const char *prev, te_string *object)
{
    const char *start = prev != NULL ? prev : array;
    unsigned int depth = 0;
    const char *pos;
    const char *open = NULL;

    for (pos = start; *pos != '\0'; pos++)
    {
        if (*pos == '{')
        {
            if (depth == 0)
                open = pos;
            depth++;
        }
        else if (*pos == '}')
        {
            if (depth == 0)
                continue;

            depth--;
            if (depth == 0 && open != NULL)
            {
                te_string_append(object, "%.*s", (int)(pos - open + 1),
                                 open);
                return pos + 1;
            }
        }
    }

    return NULL;
}
