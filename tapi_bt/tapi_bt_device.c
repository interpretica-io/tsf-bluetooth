/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief The devices around the agent
 */

#define TE_LGR_USER "TAPI BT"

#include "te_config.h"

#include <stdlib.h>
#include <string.h>

#include "logger_api.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"
#include "te_vector.h"

#include "tapi_bt_device.h"
#include "tapi_bt_internal.h"

/** Prepare an empty device with the fields that are not zero. */
static void
bt_device_init(tapi_bt_device *device)
{
    memset(device, 0, sizeof(*device));
    device->rssi = TAPI_BT_RSSI_UNKNOWN;
}

/** Append a device to a vector, taking ownership of its strings. */
static void
bt_device_add(te_vec *devices, tapi_bt_device *device)
{
    TE_VEC_APPEND(devices, *device);
    bt_device_init(device);
}

/** Run one tool and collect stdout. */
static te_errno
bt_run(tapi_job_factory_t *factory, const char *program,
       const char *const *argv, size_t argc, int timeout_ms,
       te_string *out, int *exit_code)
{
    te_vec args = TE_VEC_INIT(char *);
    size_t i;
    te_errno rc;

    for (i = 0; i < argc; i++)
        tapi_bt_arg(&args, "%s", argv[i]);

    rc = tapi_bt_cmd(factory, program, program, &args, timeout_ms, out,
                     NULL, exit_code);

    te_vec_deep_free(&args);

    return rc;
}

/**
 * Read a list of devices out of blueutil JSON.
 *
 * Verified against blueutil 2.13.0: every listing command - --paired,
 * --connected, --inquiry - answers with the same array of flat
 * objects, whose addresses are lower case with dashes.
 */
static void
bt_devices_from_blueutil(const char *json, te_vec *devices)
{
    const char *prev = NULL;
    te_string object = TE_STRING_INIT;

    while ((prev = tapi_bt_json_next(json, prev, &object)) != NULL)
    {
        const char *text = te_string_value(&object);
        tapi_bt_device device;
        te_string field = TE_STRING_INIT;

        bt_device_init(&device);

        if (tapi_bt_json_str(text, "address", &field))
            (void)tapi_bt_addr_normalize(te_string_value(&field),
                                         device.address);

        te_string_reset(&field);
        if (tapi_bt_json_str(text, "name", &field) && field.len != 0)
            device.name = TE_STRDUP(te_string_value(&field));

        (void)tapi_bt_json_bool(text, "paired", &device.paired);
        (void)tapi_bt_json_bool(text, "connected", &device.connected);
        (void)tapi_bt_json_bool(text, "favourite", &device.trusted);

        /*
         * blueutil reports RSSI only for a device that is connected,
         * and reports 0 for one in the golden range. So a missing
         * field is not a signal of zero strength, and the two are
         * kept apart.
         */
        (void)tapi_bt_json_int(text, "RSSI", &device.rssi);

        te_string_free(&field);

        if (device.address[0] != '\0')
            bt_device_add(devices, &device);
        else
            free(device.name);

        te_string_reset(&object);
    }

    te_string_free(&object);
}

/**
 * Read a list out of `bluetoothctl devices`.
 *
 * One line per device: "Device AA:BB:CC:DD:EE:FF Some Name".
 */
static void
bt_devices_from_bluetoothctl(const char *text, te_vec *devices)
{
    const char *line = text;

    while (line != NULL && *line != '\0')
    {
        const char *end = strchr(line, '\n');
        size_t len = end != NULL ? (size_t)(end - line) : strlen(line);

        if (strncmp(line, "Device ", 7) == 0)
        {
            tapi_bt_device device;
            const char *rest = line + 7;

            bt_device_init(&device);

            if (tapi_bt_addr_normalize(rest, device.address))
            {
                const char *name = rest;
                size_t name_len;

                while (*name != '\0' && *name != ' ' && name < line + len)
                    name++;
                while (*name == ' ')
                    name++;

                name_len = (size_t)(line + len - name);
                if (name_len > 0)
                {
                    device.name = TE_ALLOC(name_len + 1);
                    memcpy(device.name, name, name_len);
                }

                bt_device_add(devices, &device);
            }
        }

        line = end != NULL ? end + 1 : NULL;
    }
}

/**
 * Fill in what `bluetoothctl info` knows about one device.
 *
 * The listing gives an address and a name and nothing else, and the
 * three things a test usually wants - paired, connected, trusted - are
 * only here. One call per device, which is why it is done for the
 * known list and not while scanning.
 */
static void
bt_device_detail(tapi_job_factory_t *factory, int timeout_ms,
                 tapi_bt_device *device)
{
    const char *argv[2] = { "info", device->address };
    te_string out = TE_STRING_INIT;
    const char *text;
    int code = 0;

    if (bt_run(factory, "bluetoothctl", argv, TE_ARRAY_LEN(argv),
               timeout_ms, &out, &code) != 0 || code != 0)
    {
        te_string_free(&out);
        return;
    }

    text = te_string_value(&out);

    device->paired = strstr(text, "Paired: yes") != NULL;
    device->connected = strstr(text, "Connected: yes") != NULL;
    device->trusted = strstr(text, "Trusted: yes") != NULL;

    {
        const char *rssi = strstr(text, "RSSI:");

        if (rssi != NULL)
        {
            int value = 0;
            bool negative = false;

            rssi += strlen("RSSI:");
            while (*rssi == ' ' || *rssi == '\t')
                rssi++;
            if (*rssi == '-')
            {
                negative = true;
                rssi++;
            }
            for (; *rssi >= '0' && *rssi <= '9'; rssi++)
                value = value * 10 + (*rssi - '0');

            device->rssi = negative ? -value : value;
        }
    }

    {
        const char *class_at = strstr(text, "Class:");

        if (class_at != NULL)
        {
            device->class_of_device =
                (unsigned int)strtoul(class_at + strlen("Class:"), NULL, 0);
        }
    }

    te_string_free(&out);
}

/** The known devices on a BlueZ agent. */
static te_errno
bt_known_bluez(tapi_job_factory_t *factory, int timeout_ms, te_vec *devices)
{
    static const char *const argv[] = { "devices" };
    te_string out = TE_STRING_INIT;
    size_t i;
    int code = 0;
    te_errno rc;

    rc = bt_run(factory, "bluetoothctl", argv, TE_ARRAY_LEN(argv),
                timeout_ms, &out, &code);
    if (rc != 0 || code != 0)
    {
        te_string_free(&out);
        return rc != 0 ? rc : TE_RC(TE_TAPI, TE_ENOENT);
    }

    bt_devices_from_bluetoothctl(te_string_value(&out), devices);
    te_string_free(&out);

    for (i = 0; i < te_vec_size(devices); i++)
    {
        bt_device_detail(factory, timeout_ms,
                         (tapi_bt_device *)te_vec_get(devices, i));
    }

    return 0;
}

/** The known devices on a macOS agent. */
static te_errno
bt_known_macos(tapi_job_factory_t *factory, int timeout_ms, te_vec *devices)
{
    static const char *const paired[] = { "--paired", "--format", "json" };
    te_string out = TE_STRING_INIT;
    int code = 0;
    te_errno rc;

    rc = bt_run(factory, "blueutil", paired, TE_ARRAY_LEN(paired),
                timeout_ms, &out, &code);
    if (rc != 0 || code != 0)
    {
        ERROR("blueutil is needed to list devices on macOS and did not "
              "answer; system_profiler can be read but not asked");
        te_string_free(&out);
        return rc != 0 ? rc : TE_RC(TE_TAPI, TE_ENOENT);
    }

    bt_devices_from_blueutil(te_string_value(&out), devices);
    te_string_free(&out);

    return 0;
}

/** The known devices on a Windows agent. */
static te_errno
bt_known_windows(tapi_job_factory_t *factory, int timeout_ms,
                 te_vec *devices)
{
    te_string out = TE_STRING_INIT;
    const char *prev = NULL;
    te_string object = TE_STRING_INIT;
    int code = 0;
    te_errno rc;

    /*
     * BTHENUM is where Windows puts a paired device: the instance id
     * carries the address as twelve hex digits after the last
     * underscore. Status 'OK' means the device is present, which for
     * Bluetooth means connected; a paired device that is switched off
     * is 'Unknown'.
     */
    rc = tapi_bt_powershell(factory,
        "@(Get-PnpDevice -Class Bluetooth -ErrorAction SilentlyContinue | "
        "Where-Object { $_.InstanceId -like 'BTHENUM*' } | ForEach-Object "
        "{ [pscustomobject]@{ address = "
        "$($_.InstanceId -replace '.*_','' ); "
        "name = $_.FriendlyName; connected = ($_.Status -eq 'OK'); "
        "paired = $true } }) | ConvertTo-Json -Compress -AsArray",
        timeout_ms, &out, &code);

    if (rc != 0)
    {
        te_string_free(&out);
        return rc;
    }

    while ((prev = tapi_bt_json_next(te_string_value(&out), prev,
                                     &object)) != NULL)
    {
        const char *text = te_string_value(&object);
        tapi_bt_device device;
        te_string field = TE_STRING_INIT;

        bt_device_init(&device);

        if (tapi_bt_json_str(text, "address", &field))
            (void)tapi_bt_addr_normalize(te_string_value(&field),
                                         device.address);

        te_string_reset(&field);
        if (tapi_bt_json_str(text, "name", &field) && field.len != 0)
            device.name = TE_STRDUP(te_string_value(&field));

        (void)tapi_bt_json_bool(text, "paired", &device.paired);
        (void)tapi_bt_json_bool(text, "connected", &device.connected);

        te_string_free(&field);

        if (device.address[0] != '\0')
            bt_device_add(devices, &device);
        else
            free(device.name);

        te_string_reset(&object);
    }

    te_string_free(&out);
    te_string_free(&object);

    return 0;
}

/* See description in tapi_bt_device.h */
te_errno
tapi_bt_known(tapi_job_factory_t *factory, tapi_bt_backend backend,
              int timeout_ms, te_vec *devices)
{
    tapi_bt_backend actual = backend;
    te_errno rc;

    *devices = (te_vec)TE_VEC_INIT(tapi_bt_device);

    if (actual == TAPI_BT_AUTO)
    {
        rc = tapi_bt_detect(factory, timeout_ms, &actual);
        if (rc != 0)
            return rc;
    }

    if (!tapi_bt_supports(actual, TAPI_BT_FEAT_KNOWN))
    {
        ERROR("%s keeps no list of devices it has met; that lives in "
              "bluetoothd, and this agent has the kernel and not the "
              "daemon. Scan instead.", tapi_bt_backend2str(actual));
        return TE_RC(TE_TAPI, TE_EOPNOTSUPP);
    }

    switch (actual)
    {
        case TAPI_BT_BLUEZ:
            rc = bt_known_bluez(factory, timeout_ms, devices);
            break;
        case TAPI_BT_MACOS:
            rc = bt_known_macos(factory, timeout_ms, devices);
            break;
        case TAPI_BT_WINDOWS:
            rc = bt_known_windows(factory, timeout_ms, devices);
            break;
        default:
            rc = TE_RC(TE_TAPI, TE_EOPNOTSUPP);
            break;
    }

    if (rc != 0)
        tapi_bt_devices_free(devices);

    return rc;
}

/* See description in tapi_bt_device.h */
const tapi_bt_device *
tapi_bt_device_find(const te_vec *devices, const char *address)
{
    char wanted[TAPI_BT_ADDR_LEN];
    size_t i;

    if (!tapi_bt_addr_normalize(address, wanted))
        return NULL;

    for (i = 0; i < te_vec_size(devices); i++)
    {
        const tapi_bt_device *device = te_vec_get((te_vec *)devices, i);

        if (strcmp(device->address, wanted) == 0)
            return device;
    }

    return NULL;
}

/** One device operation, spelled differently on each system. */
static te_errno
bt_device_op(tapi_job_factory_t *factory, tapi_bt_backend backend,
             unsigned int feature, const char *op, const char *address,
             int timeout_ms)
{
    char normalized[TAPI_BT_ADDR_LEN];
    tapi_bt_backend actual = backend;
    te_string out = TE_STRING_INIT;
    int code = 0;
    te_errno rc;

    if (!tapi_bt_addr_normalize(address, normalized))
    {
        ERROR("'%s' is not a Bluetooth address", address);
        return TE_RC(TE_TAPI, TE_EINVAL);
    }

    if (actual == TAPI_BT_AUTO)
    {
        rc = tapi_bt_detect(factory, timeout_ms, &actual);
        if (rc != 0)
            return rc;
    }

    if (!tapi_bt_supports(actual, feature))
    {
        ERROR("%s cannot %s a device from a test: pairing on that system "
              "needs a person to agree to it",
              tapi_bt_backend2str(actual), op);
        return TE_RC(TE_TAPI, TE_EOPNOTSUPP);
    }

    switch (actual)
    {
        case TAPI_BT_BLUEZ:
        {
            const char *argv[2] = { op, normalized };

            rc = bt_run(factory, "bluetoothctl", argv, TE_ARRAY_LEN(argv),
                        timeout_ms, &out, &code);
            break;
        }

        case TAPI_BT_MACOS:
        {
            te_string flag = TE_STRING_INIT;
            const char *argv[2];

            te_string_append(&flag, "--%s", op);
            argv[0] = flag.ptr;
            argv[1] = normalized;

            rc = bt_run(factory, "blueutil", argv, TE_ARRAY_LEN(argv),
                        timeout_ms, &out, &code);
            te_string_free(&flag);
            break;
        }

        default:
            rc = TE_RC(TE_TAPI, TE_EOPNOTSUPP);
            break;
    }

    if (rc == 0 && code != 0)
    {
        ERROR("%s %s failed (status %d): %s", op, normalized, code,
              te_string_value(&out));
        rc = TE_RC(TE_TAPI, TE_ESHCMD);
    }

    te_string_free(&out);

    return rc;
}

/* See description in tapi_bt_device.h */
te_errno
tapi_bt_pair(tapi_job_factory_t *factory, tapi_bt_backend backend,
             const char *address, int timeout_ms)
{
    return bt_device_op(factory, backend, TAPI_BT_FEAT_PAIR, "pair",
                        address, timeout_ms);
}

/* See description in tapi_bt_device.h */
te_errno
tapi_bt_remove(tapi_job_factory_t *factory, tapi_bt_backend backend,
               const char *address, int timeout_ms)
{
    tapi_bt_backend actual = backend;
    te_errno rc;

    if (actual == TAPI_BT_AUTO)
    {
        rc = tapi_bt_detect(factory, timeout_ms, &actual);
        if (rc != 0)
            return rc;
    }

    /* BlueZ calls it remove, blueutil calls it unpair. */
    return bt_device_op(factory, actual, TAPI_BT_FEAT_PAIR,
                        actual == TAPI_BT_MACOS ? "unpair" : "remove",
                        address, timeout_ms);
}

/* See description in tapi_bt_device.h */
te_errno
tapi_bt_connect(tapi_job_factory_t *factory, tapi_bt_backend backend,
                const char *address, bool connect, int timeout_ms)
{
    return bt_device_op(factory, backend, TAPI_BT_FEAT_CONNECT,
                        connect ? "connect" : "disconnect", address,
                        timeout_ms);
}

/* See description in tapi_bt_device.h */
void
tapi_bt_devices_log(const char *what, const te_vec *devices)
{
    size_t i;

    RING("%zu devices %s", te_vec_size(devices), what);

    for (i = 0; i < te_vec_size(devices); i++)
    {
        const tapi_bt_device *device = te_vec_get((te_vec *)devices, i);
        te_string flags = TE_STRING_INIT;

        if (device->paired)
            te_string_append(&flags, " paired");
        if (device->connected)
            te_string_append(&flags, " connected");
        if (device->trusted)
            te_string_append(&flags, " trusted");

        if (device->rssi != TAPI_BT_RSSI_UNKNOWN)
            te_string_append(&flags, " %d dBm", device->rssi);

        RING("  %s %s%s", device->address,
             device->name != NULL ? device->name : "(no name)",
             te_string_value(&flags));

        te_string_free(&flags);
    }
}

/* See description in tapi_bt_device.h */
void
tapi_bt_devices_free(te_vec *devices)
{
    size_t i;

    for (i = 0; i < te_vec_size(devices); i++)
    {
        tapi_bt_device *device = te_vec_get(devices, i);

        free(device->name);
        free(device->detail);
    }

    te_vec_free(devices);
}

/**
 * Read `btmgmt find` output.
 *
 * The format, taken from the binary:
 *
 *     hci0 dev_found: AA:BB:CC:DD:EE:FF type LE Random rssi -80 flags 0x0
 *
 * One line per sighting, and a device seen twice is on two lines, so
 * the address is checked against what is already collected.
 */
static void
bt_devices_from_btmgmt(const char *text, te_vec *devices)
{
    const char *line = text;

    while (line != NULL && *line != '\0')
    {
        const char *end = strchr(line, '\n');
        const char *found = strstr(line, "dev_found:");

        if (found != NULL && (end == NULL || found < end))
        {
            tapi_bt_device device;
            const char *type;
            const char *rssi;

            bt_device_init(&device);
            found += strlen("dev_found:");

            if (tapi_bt_addr_normalize(found, device.address) &&
                tapi_bt_device_find(devices, device.address) == NULL)
            {
                rssi = strstr(found, "rssi ");
                if (rssi != NULL && (end == NULL || rssi < end))
                {
                    device.rssi = (int)strtol(rssi + strlen("rssi "), NULL,
                                              10);
                }

                /*
                 * "type LE Random", "type LE Public", "type BR/EDR".
                 * Kept as text because it is what the log wants and
                 * because the address type matters to a reader: a
                 * random LE address is a different thing from a public
                 * one and will not be there tomorrow.
                 */
                type = strstr(found, "type ");
                if (type != NULL && (end == NULL || type < end))
                {
                    const char *stop = strstr(type, " rssi");
                    size_t len;

                    if (stop == NULL || (end != NULL && stop > end))
                        stop = end != NULL ? end : type + strlen(type);

                    len = (size_t)(stop - type);
                    device.detail = TE_ALLOC(len + 1);
                    memcpy(device.detail, type, len);
                }

                bt_device_add(devices, &device);
            }
            else
            {
                free(device.name);
            }
        }

        line = end != NULL ? end + 1 : NULL;
    }
}

/**
 * Read `hcitool scan` output.
 *
 *     Scanning ...
 *     	AA:BB:CC:DD:EE:FF	Some Name
 */
static void
bt_devices_from_hcitool(const char *text, te_vec *devices)
{
    const char *line = strchr(text, '\n');

    while (line != NULL)
    {
        const char *end;
        tapi_bt_device device;

        line++;
        end = strchr(line, '\n');

        while (*line == ' ' || *line == '\t')
            line++;

        bt_device_init(&device);

        if (tapi_bt_addr_normalize(line, device.address) &&
            tapi_bt_device_find(devices, device.address) == NULL)
        {
            const char *name = strchr(line, '\t');

            if (name != NULL && (end == NULL || name < end))
            {
                size_t len;

                name++;
                len = end != NULL ? (size_t)(end - name) : strlen(name);
                if (len > 0)
                {
                    device.name = TE_ALLOC(len + 1);
                    memcpy(device.name, name, len);
                }
            }

            bt_device_add(devices, &device);
        }
        else
        {
            free(device.name);
        }

        line = end;
    }
}

/* See description in tapi_bt_device.h */
te_errno
tapi_bt_scan(tapi_job_factory_t *factory, tapi_bt_backend backend,
             unsigned int seconds, int timeout_ms, te_vec *devices)
{
    tapi_bt_backend actual = backend;
    te_string out = TE_STRING_INIT;
    te_string duration = TE_STRING_INIT;
    int code = 0;
    te_errno rc;

    *devices = (te_vec)TE_VEC_INIT(tapi_bt_device);

    if (actual == TAPI_BT_AUTO)
    {
        rc = tapi_bt_detect(factory, timeout_ms, &actual);
        if (rc != 0)
            return rc;
    }

    if (seconds == 0)
        seconds = 10;

    te_string_append(&duration, "%u", seconds);

    RING("Looking for devices in range for %u s through %s", seconds,
         tapi_bt_backend2str(actual));

    switch (actual)
    {
        case TAPI_BT_BLUEZ:
        case TAPI_BT_HCI:
        {
            const char *mgmt[3] = { "--timeout", duration.ptr, "find" };
            static const char *const hci[] = { "scan" };

            /*
             * btmgmt first: it looks for LE as well as classic and it
             * reports a signal strength, neither of which hcitool scan
             * does. hcitool is the fallback for an image whose kernel
             * has no management socket.
             */
            rc = bt_run(factory, "btmgmt", mgmt, TE_ARRAY_LEN(mgmt),
                        timeout_ms, &out, &code);
            if (rc == 0 && code == 0 &&
                strstr(te_string_value(&out), "dev_found") != NULL)
            {
                bt_devices_from_btmgmt(te_string_value(&out), devices);
                break;
            }

            te_string_reset(&out);
            rc = bt_run(factory, "hcitool", hci, TE_ARRAY_LEN(hci),
                        timeout_ms, &out, &code);
            if (rc == 0 && code == 0)
                bt_devices_from_hcitool(te_string_value(&out), devices);
            break;
        }

        case TAPI_BT_MACOS:
        {
            const char *argv[4] = {
                "--inquiry", duration.ptr, "--format", "json",
            };

            rc = bt_run(factory, "blueutil", argv, TE_ARRAY_LEN(argv),
                        timeout_ms, &out, &code);
            if (rc == 0 && code == 0)
                bt_devices_from_blueutil(te_string_value(&out), devices);
            break;
        }

        case TAPI_BT_WINDOWS:
        {
            te_string script = TE_STRING_INIT;

            /*
             * WinRT, because the PnP tree only knows what has been
             * paired. GetDeviceSelectorFromPairingState($false) asks
             * the enumeration for devices that are present and not
             * paired, which is Windows' idea of "in range".
             *
             * Unverified: written from the WinRT documentation, with
             * no Windows machine to run it on. The first suite to use
             * this should expect to correct it.
             */
            te_string_append(&script,
                "$null = [Windows.Devices.Enumeration.DeviceInformation,"
                "Windows.Devices.Enumeration,ContentType=WindowsRuntime]; "
                "$null = [Windows.Devices.Bluetooth.BluetoothDevice,"
                "Windows.Devices.Bluetooth,ContentType=WindowsRuntime]; "
                "$sel = [Windows.Devices.Bluetooth.BluetoothDevice]::"
                "GetDeviceSelectorFromPairingState($false); "
                "$op = [Windows.Devices.Enumeration.DeviceInformation]::"
                "FindAllAsync($sel); "
                "while ($op.Status -eq 0) { Start-Sleep -Milliseconds 200 }"
                "; Start-Sleep -Seconds %u; "
                "@($op.GetResults() | ForEach-Object { "
                "[pscustomobject]@{ address = $($_.Id -replace '.*-','');"
                " name = $_.Name; paired = $false; connected = $false } })"
                " | ConvertTo-Json -Compress -AsArray", seconds);

            rc = tapi_bt_powershell(factory, script.ptr, timeout_ms, &out,
                                    &code);
            if (rc == 0)
                bt_devices_from_blueutil(te_string_value(&out), devices);

            te_string_free(&script);
            break;
        }

        default:
            rc = TE_RC(TE_TAPI, TE_EINVAL);
            break;
    }

    te_string_free(&out);
    te_string_free(&duration);

    if (rc != 0)
        tapi_bt_devices_free(devices);

    return rc;
}
