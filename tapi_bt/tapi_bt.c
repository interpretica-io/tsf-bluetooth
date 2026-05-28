/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Bluetooth from a test
 *
 * One function per operation and a switch over the backend inside it.
 * The switch is deliberate: the four systems have almost nothing in
 * common at this level, and a table of command templates would hide
 * which of them cannot do a thing at all.
 */

#define TE_LGR_USER "TAPI BT"

#include "te_config.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "logger_api.h"
#include "te_alloc.h"
#include "te_str.h"
#include "te_string.h"
#include "te_vector.h"

#include "tapi_bt.h"
#include "tapi_bt_internal.h"

/* See description in tapi_bt.h */
const char *
tapi_bt_backend2str(tapi_bt_backend backend)
{
    switch (backend)
    {
        case TAPI_BT_BLUEZ:
            return "bluez";
        case TAPI_BT_HCI:
            return "hci";
        case TAPI_BT_MACOS:
            return "macos";
        case TAPI_BT_WINDOWS:
            return "windows";
        default:
            return "auto";
    }
}

/* See description in tapi_bt.h */
unsigned int
tapi_bt_features(tapi_bt_backend backend)
{
    switch (backend)
    {
        case TAPI_BT_BLUEZ:
            /* The daemon has all of it. */
            return TAPI_BT_FEAT_ADAPTER | TAPI_BT_FEAT_POWER |
                   TAPI_BT_FEAT_DISCOVERABLE | TAPI_BT_FEAT_SCAN |
                   TAPI_BT_FEAT_KNOWN | TAPI_BT_FEAT_PAIR |
                   TAPI_BT_FEAT_CONNECT | TAPI_BT_FEAT_RSSI |
                   TAPI_BT_FEAT_HCI_TRACE | TAPI_BT_FEAT_L2_SDP;

        case TAPI_BT_HCI:
            /*
             * The kernel knows about the link and nothing about what
             * is done over it: pairing, bonding and the device list
             * live in bluetoothd, so an image without the daemon has
             * the radio and no memory of anything it met.
             */
            return TAPI_BT_FEAT_ADAPTER | TAPI_BT_FEAT_POWER |
                   TAPI_BT_FEAT_DISCOVERABLE | TAPI_BT_FEAT_SCAN |
                   TAPI_BT_FEAT_RSSI | TAPI_BT_FEAT_HCI_TRACE |
                   TAPI_BT_FEAT_L2_SDP;

        case TAPI_BT_MACOS:
            /*
             * No HCI: macOS keeps the link to itself, and the only way
             * to see it is PacketLogger, which is a separate download
             * and not a thing a test can rely on. No l2ping or SDP
             * browse either - those are BlueZ tools.
             */
            return TAPI_BT_FEAT_ADAPTER | TAPI_BT_FEAT_POWER |
                   TAPI_BT_FEAT_DISCOVERABLE | TAPI_BT_FEAT_SCAN |
                   TAPI_BT_FEAT_KNOWN | TAPI_BT_FEAT_PAIR |
                   TAPI_BT_FEAT_CONNECT | TAPI_BT_FEAT_RSSI;

        case TAPI_BT_WINDOWS:
            /*
             * Windows does not give a program the HCI link at all -
             * the stack owns it, and what is offered instead is an ETW
             * trace, which is a different thing and is not pretended
             * to be this one. Pairing is possible through WinRT but
             * needs a ceremony with a consent callback that a headless
             * PowerShell line cannot answer, so it is not offered
             * either.
             */
            return TAPI_BT_FEAT_ADAPTER | TAPI_BT_FEAT_POWER |
                   TAPI_BT_FEAT_SCAN | TAPI_BT_FEAT_KNOWN;

        default:
            return 0;
    }
}

/* See description in tapi_bt.h */
bool
tapi_bt_supports(tapi_bt_backend backend, unsigned int feature)
{
    return (tapi_bt_features(backend) & feature) == feature;
}

/* See description in tapi_bt.h */
bool
tapi_bt_addr_normalize(const char *address, char *dest)
{
    unsigned int octets = 0;
    size_t used = 0;

    if (address == NULL)
        return false;

    while (*address != '\0' && octets < 6)
    {
        int high;
        int low;

        while (*address == ':' || *address == '-' || *address == ' ')
            address++;

        if (!isxdigit((unsigned char)address[0]) ||
            !isxdigit((unsigned char)address[1]))
        {
            break;
        }

        high = toupper((unsigned char)address[0]);
        low = toupper((unsigned char)address[1]);
        address += 2;

        if (octets != 0)
            dest[used++] = ':';

        dest[used++] = (char)high;
        dest[used++] = (char)low;
        octets++;
    }

    dest[used] = '\0';

    return octets == 6;
}

/** Run one tool with a fixed argument list and collect stdout. */
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
 * Does `hcitool dev` name an adapter?
 *
 * It prints a header and then one indented line per adapter, and it
 * exits 0 whether or not there are any - measured on BlueZ 5.72 with
 * no Bluetooth in the kernel at all, where it still printed
 * "Devices:" and returned success. So the list is what gets read.
 */
static bool
bt_hcitool_has_device(const char *text)
{
    const char *line = strchr(text, '\n');

    while (line != NULL)
    {
        line++;
        while (*line == ' ' || *line == '\t')
            line++;

        if (strncmp(line, "hci", 3) == 0)
            return true;

        line = strchr(line, '\n');
    }

    return false;
}

/** Is this a Linux with bluetoothd answering? */
static bool
bt_probe_bluez(tapi_job_factory_t *factory, int timeout_ms)
{
    static const char *const argv[] = { "--timeout", "2", "list" };
    te_string out = TE_STRING_INIT;
    int code = 0;
    bool found;

    /*
     * Only ever asked after btmgmt has said there is an adapter,
     * because without bluetoothd this does not fail - it aborts out of
     * libdbus, and a SIGABRT is not an answer.
     */
    if (bt_run(factory, "bluetoothctl", argv, TE_ARRAY_LEN(argv),
               timeout_ms, &out, &code) != 0)
    {
        te_string_free(&out);
        return false;
    }

    found = code == 0 && strstr(te_string_value(&out), "Controller") != NULL;
    te_string_free(&out);

    return found;
}

/** Is this a Linux with an HCI adapter? */
static bool
bt_probe_hci(tapi_job_factory_t *factory, int timeout_ms)
{
    static const char *const mgmt[] = { "info" };
    static const char *const dev[] = { "dev" };
    te_string out = TE_STRING_INIT;
    int code = 0;
    bool found = false;

    /* btmgmt fails honestly when there is nothing to talk to. */
    if (bt_run(factory, "btmgmt", mgmt, TE_ARRAY_LEN(mgmt), timeout_ms,
               &out, &code) == 0 && code == 0 &&
        strstr(te_string_value(&out), "addr ") != NULL)
    {
        te_string_free(&out);
        return true;
    }

    te_string_reset(&out);

    if (bt_run(factory, "hcitool", dev, TE_ARRAY_LEN(dev), timeout_ms,
               &out, &code) == 0 && code == 0)
    {
        found = bt_hcitool_has_device(te_string_value(&out));
    }

    te_string_free(&out);

    return found;
}

/** Is this a macOS with a controller? */
static bool
bt_probe_macos(tapi_job_factory_t *factory, int timeout_ms)
{
    static const char *const argv[] = { "SPBluetoothDataType", "-json" };
    te_string out = TE_STRING_INIT;
    int code = 0;
    bool found;

    /*
     * system_profiler rather than blueutil: it is part of the system,
     * so its absence means this is not a Mac, where blueutil's absence
     * would only mean nobody installed it.
     */
    if (bt_run(factory, "system_profiler", argv, TE_ARRAY_LEN(argv),
               timeout_ms, &out, &code) != 0)
    {
        te_string_free(&out);
        return false;
    }

    found = code == 0 &&
            strstr(te_string_value(&out), "controller_address") != NULL;
    te_string_free(&out);

    return found;
}

/** Is this a Windows with a Bluetooth radio? */
static bool
bt_probe_windows(tapi_job_factory_t *factory, int timeout_ms)
{
    te_string out = TE_STRING_INIT;
    int code = 0;
    bool found;

    /*
     * The PnP tree rather than WinRT: it answers from plain
     * PowerShell, where the radio API needs an async ceremony that a
     * single -Command line cannot run.
     */
    if (tapi_bt_powershell(factory,
            "$d = Get-PnpDevice -Class Bluetooth -ErrorAction "
            "SilentlyContinue | Where-Object { $_.Status -eq 'OK' }; "
            "if ($d) { 'TAPI_BT_PRESENT' }",
            timeout_ms, &out, &code) != 0)
    {
        te_string_free(&out);
        return false;
    }

    found = strstr(te_string_value(&out), "TAPI_BT_PRESENT") != NULL;
    te_string_free(&out);

    return found;
}

/* See description in tapi_bt.h */
te_errno
tapi_bt_detect(tapi_job_factory_t *factory, int timeout_ms,
               tapi_bt_backend *backend)
{
    /*
     * HCI before BlueZ on purpose. btmgmt answers from the kernel and
     * says plainly whether an adapter exists; bluetoothctl is only
     * worth asking once something has said one does, because without
     * the daemon it does not answer at all - it aborts.
     */
    if (bt_probe_hci(factory, timeout_ms))
    {
        *backend = bt_probe_bluez(factory, timeout_ms) ?
                   TAPI_BT_BLUEZ : TAPI_BT_HCI;
        RING("The agent has Bluetooth through %s",
             tapi_bt_backend2str(*backend));
        return 0;
    }

    if (bt_probe_macos(factory, timeout_ms))
    {
        *backend = TAPI_BT_MACOS;
        RING("The agent has Bluetooth through macOS");
        return 0;
    }

    if (bt_probe_windows(factory, timeout_ms))
    {
        *backend = TAPI_BT_WINDOWS;
        RING("The agent has Bluetooth through Windows");
        return 0;
    }

    RING("The agent has no Bluetooth adapter this library can reach");

    return TE_RC(TE_TAPI, TE_ENOENT);
}

/** Resolve TAPI_BT_AUTO, or take the caller's word for it. */
static te_errno
bt_backend(tapi_job_factory_t *factory, tapi_bt_backend requested,
           int timeout_ms, tapi_bt_backend *backend)
{
    if (requested != TAPI_BT_AUTO)
    {
        *backend = requested;
        return 0;
    }

    return tapi_bt_detect(factory, timeout_ms, backend);
}

/* See description in tapi_bt.h */
bool
tapi_bt_available(tapi_job_factory_t *factory, tapi_bt_backend backend,
                  int timeout_ms)
{
    switch (backend)
    {
        case TAPI_BT_AUTO:
        {
            tapi_bt_backend found;

            return tapi_bt_detect(factory, timeout_ms, &found) == 0;
        }

        case TAPI_BT_BLUEZ:
            return bt_probe_hci(factory, timeout_ms) &&
                   bt_probe_bluez(factory, timeout_ms);

        case TAPI_BT_HCI:
            return bt_probe_hci(factory, timeout_ms);

        case TAPI_BT_MACOS:
            return bt_probe_macos(factory, timeout_ms);

        case TAPI_BT_WINDOWS:
            return bt_probe_windows(factory, timeout_ms);
    }

    return false;
}

/** Value of a "key value" line in btmgmt/hciconfig output. */
static char *
bt_line_field(const char *text, const char *key, const char *stop)
{
    const char *found = strstr(text, key);
    size_t len;
    char *value;

    if (found == NULL)
        return NULL;

    found += strlen(key);
    while (*found == ' ' || *found == '\t' || *found == '\'')
        found++;

    len = strcspn(found, stop);
    value = TE_ALLOC(len + 1);
    memcpy(value, found, len);

    while (len > 0 && (value[len - 1] == ' ' || value[len - 1] == '\r' ||
                       value[len - 1] == '\''))
    {
        value[--len] = '\0';
    }

    return value;
}

/**
 * Read the adapter through btmgmt.
 *
 * The format, taken from the binary rather than from memory:
 *
 *     hci0:	Primary controller
 *     	addr AA:BB:CC:DD:EE:FF version 11 manufacturer 2 class 0x1c010c
 *     	supported settings: powered connectable ...
 *     	current settings: powered bondable ssp br/edr le secure-conn
 *     	name somehost
 */
static te_errno
bt_adapter_btmgmt(tapi_job_factory_t *factory, int timeout_ms,
                  tapi_bt_adapter *adapter)
{
    static const char *const argv[] = { "info" };
    te_string out = TE_STRING_INIT;
    char *addr;
    char *settings;
    int code = 0;
    te_errno rc;

    rc = bt_run(factory, "btmgmt", argv, TE_ARRAY_LEN(argv), timeout_ms,
                &out, &code);
    if (rc != 0)
    {
        te_string_free(&out);
        return rc;
    }

    if (code != 0)
    {
        te_string_free(&out);
        return TE_RC(TE_TAPI, TE_ENOENT);
    }

    addr = bt_line_field(te_string_value(&out), "addr ", " \t\n");
    if (addr != NULL)
    {
        (void)tapi_bt_addr_normalize(addr, adapter->address);
        free(addr);
    }

    adapter->name = bt_line_field(te_string_value(&out), "\tname ", "\n");
    adapter->identifier = bt_line_field(te_string_value(&out), "hci",
                                        ":\t \n");
    if (adapter->identifier != NULL)
    {
        te_string ident = TE_STRING_INIT;

        te_string_append(&ident, "hci%s", adapter->identifier);
        free(adapter->identifier);
        adapter->identifier = ident.ptr;
    }

    settings = bt_line_field(te_string_value(&out), "current settings:",
                             "\n");
    if (settings != NULL)
    {
        /*
         * One line carries the whole posture, which is why it is kept
         * whole in detail: tapi_bt_audit reads it again for the
         * findings, and a reader of the log needs to see what it read.
         */
        adapter->powered = strstr(settings, "powered") != NULL;
        adapter->discoverable = strstr(settings, "discoverable") != NULL;
        adapter->pairable = strstr(settings, "bondable") != NULL;
        adapter->detail = settings;
    }

    te_string_free(&out);

    return adapter->address[0] != '\0' ? 0 : TE_RC(TE_TAPI, TE_ENOENT);
}

/**
 * Read the adapter through hciconfig.
 *
 *     hci0:	Type: Primary  Bus: USB
 *     	BD Address: AA:BB:CC:DD:EE:FF  ACL MTU: 1021:8  SCO MTU: 64:1
 *     	UP RUNNING PSCAN ISCAN
 *     	Name: 'somehost'
 */
static te_errno
bt_adapter_hciconfig(tapi_job_factory_t *factory, int timeout_ms,
                     tapi_bt_adapter *adapter)
{
    static const char *const argv[] = { "-a" };
    te_string out = TE_STRING_INIT;
    const char *text;
    char *addr;
    int code = 0;
    te_errno rc;

    rc = bt_run(factory, "hciconfig", argv, TE_ARRAY_LEN(argv), timeout_ms,
                &out, &code);
    if (rc != 0 || code != 0)
    {
        te_string_free(&out);
        return rc != 0 ? rc : TE_RC(TE_TAPI, TE_ENOENT);
    }

    text = te_string_value(&out);

    addr = bt_line_field(text, "BD Address:", " \t\n");
    if (addr != NULL)
    {
        (void)tapi_bt_addr_normalize(addr, adapter->address);
        free(addr);
    }

    adapter->name = bt_line_field(text, "Name:", "\n");
    adapter->identifier = bt_line_field(text, "", ":");

    /*
     * The flag line: UP or DOWN, then the scan modes. PSCAN is page
     * scan - it answers a connection from something that knows the
     * address - and ISCAN is inquiry scan, which is what makes the
     * adapter findable by a stranger. They are different questions and
     * only the second one is "discoverable".
     */
    adapter->powered = strstr(text, "\tUP ") != NULL ||
                       strstr(text, "\tUP\n") != NULL;
    adapter->discoverable = strstr(text, "ISCAN") != NULL;
    adapter->pairable = strstr(text, "PSCAN") != NULL;
    adapter->detail = bt_line_field(text, "HCI Version:", "\n");

    te_string_free(&out);

    return adapter->address[0] != '\0' ? 0 : TE_RC(TE_TAPI, TE_ENOENT);
}

/** Read the adapter on macOS. */
static te_errno
bt_adapter_macos(tapi_job_factory_t *factory, int timeout_ms,
                 tapi_bt_adapter *adapter)
{
    static const char *const argv[] = { "SPBluetoothDataType", "-json" };
    te_string out = TE_STRING_INIT;
    te_string field = TE_STRING_INIT;
    const char *text;
    int code = 0;
    te_errno rc;

    rc = bt_run(factory, "system_profiler", argv, TE_ARRAY_LEN(argv),
                timeout_ms, &out, &code);
    if (rc != 0 || code != 0)
    {
        te_string_free(&out);
        return rc != 0 ? rc : TE_RC(TE_TAPI, TE_ENOENT);
    }

    text = te_string_value(&out);

    if (tapi_bt_json_str(text, "controller_address", &field))
        (void)tapi_bt_addr_normalize(te_string_value(&field), adapter->address);

    /*
     * "attrib_on" and "attrib_off", which is how system_profiler
     * spells a boolean. There is no separate pairable state: macOS
     * accepts a pairing attempt whenever the radio is on.
     */
    te_string_reset(&field);
    if (tapi_bt_json_str(text, "controller_state", &field))
        adapter->powered = strstr(te_string_value(&field), "_on") != NULL;

    te_string_reset(&field);
    if (tapi_bt_json_str(text, "controller_discoverable", &field))
    {
        adapter->discoverable =
            strstr(te_string_value(&field), "_on") != NULL;
    }

    adapter->pairable = adapter->powered;

    te_string_reset(&field);
    if (tapi_bt_json_str(text, "controller_chipset", &field))
        adapter->identifier = TE_STRDUP(te_string_value(&field));

    te_string_reset(&field);
    if (tapi_bt_json_str(text, "controller_supportedServices", &field))
        adapter->detail = TE_STRDUP(te_string_value(&field));

    /*
     * system_profiler does not report the host's own Bluetooth name;
     * it is the computer name, which scutil knows.
     */
    {
        static const char *const name_argv[] = { "--get", "ComputerName" };
        te_string name = TE_STRING_INIT;
        int name_code = 0;

        if (bt_run(factory, "scutil", name_argv, TE_ARRAY_LEN(name_argv),
                   timeout_ms, &name, &name_code) == 0 && name_code == 0)
        {
            char *trimmed = te_str_strip_spaces(te_string_value(&name));

            if (trimmed != NULL && trimmed[0] != '\0')
                adapter->name = trimmed;
            else
                free(trimmed);
        }

        te_string_free(&name);
    }

    te_string_free(&out);
    te_string_free(&field);

    return adapter->address[0] != '\0' ? 0 : TE_RC(TE_TAPI, TE_ENOENT);
}

/** Read the adapter on Windows. */
static te_errno
bt_adapter_windows(tapi_job_factory_t *factory, int timeout_ms,
                   tapi_bt_adapter *adapter)
{
    te_string out = TE_STRING_INIT;
    te_string field = TE_STRING_INIT;
    const char *text;
    int code = 0;
    te_errno rc;

    /*
     * One PowerShell line producing one flat JSON object, because
     * that is the only shape this library parses and because a
     * multi-statement script quoted through a job's argument vector is
     * a quoting problem waiting to happen.
     *
     * The radio's own address is not in the PnP tree. Windows keeps it
     * under BTHPORT\Parameters\Keys, whose subkey name is the local
     * address - readable only with administrator rights, so it comes
     * back empty for an ordinary agent and the adapter is reported
     * without one rather than not at all.
     */
    rc = tapi_bt_powershell(factory,
        "$r = Get-PnpDevice -Class Bluetooth -ErrorAction SilentlyContinue "
        "| Where-Object { $_.InstanceId -like 'USB*' -or $_.InstanceId "
        "-like 'PCI*' -or $_.InstanceId -like 'BTH\\MS*' } "
        "| Select-Object -First 1; "
        "$a = ''; try { $k = Get-ChildItem "
        "'HKLM:\\SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters"
        "\\Keys' -ErrorAction Stop | Select-Object -First 1; "
        "$a = $k.PSChildName } catch { }; "
        "[pscustomobject]@{ name = $env:COMPUTERNAME; "
        "id = $(if ($r) { $r.InstanceId } else { '' }); "
        "friendly = $(if ($r) { $r.FriendlyName } else { '' }); "
        "powered = $(if ($r -and $r.Status -eq 'OK') { $true } "
        "else { $false }); address = $a } | ConvertTo-Json -Compress",
        timeout_ms, &out, &code);

    if (rc != 0)
    {
        te_string_free(&out);
        return rc;
    }

    text = te_string_value(&out);

    if (tapi_bt_json_str(text, "address", &field) && field.len != 0)
        (void)tapi_bt_addr_normalize(te_string_value(&field), adapter->address);

    te_string_reset(&field);
    if (tapi_bt_json_str(text, "name", &field))
        adapter->name = TE_STRDUP(te_string_value(&field));

    te_string_reset(&field);
    if (tapi_bt_json_str(text, "id", &field))
        adapter->identifier = TE_STRDUP(te_string_value(&field));

    te_string_reset(&field);
    if (tapi_bt_json_str(text, "friendly", &field))
        adapter->detail = TE_STRDUP(te_string_value(&field));

    (void)tapi_bt_json_bool(text, "powered", &adapter->powered);

    /*
     * Windows has no notion a program can read of being discoverable
     * or pairable: the Settings app owns that, and there is no API
     * that reports it. Left false rather than guessed, and
     * TAPI_BT_FEAT_DISCOVERABLE says so.
     */
    adapter->discoverable = false;
    adapter->pairable = false;

    te_string_free(&out);
    te_string_free(&field);

    return adapter->identifier != NULL && adapter->identifier[0] != '\0' ?
           0 : TE_RC(TE_TAPI, TE_ENOENT);
}

/* See description in tapi_bt.h */
te_errno
tapi_bt_adapter_get(tapi_job_factory_t *factory, tapi_bt_backend backend,
                    int timeout_ms, tapi_bt_adapter *adapter)
{
    tapi_bt_backend actual;
    te_errno rc;

    memset(adapter, 0, sizeof(*adapter));

    rc = bt_backend(factory, backend, timeout_ms, &actual);
    if (rc != 0)
        return rc;

    adapter->backend = actual;

    switch (actual)
    {
        case TAPI_BT_BLUEZ:
        case TAPI_BT_HCI:
            /*
             * btmgmt first: it reports the settings as words, which is
             * both more and more exactly what is wanted than
             * hciconfig's flag line. hciconfig is the fallback for a
             * kernel or a build where the management socket is not
             * there.
             */
            rc = bt_adapter_btmgmt(factory, timeout_ms, adapter);
            if (rc != 0)
                rc = bt_adapter_hciconfig(factory, timeout_ms, adapter);
            break;

        case TAPI_BT_MACOS:
            rc = bt_adapter_macos(factory, timeout_ms, adapter);
            break;

        case TAPI_BT_WINDOWS:
            rc = bt_adapter_windows(factory, timeout_ms, adapter);
            break;

        default:
            rc = TE_RC(TE_TAPI, TE_EINVAL);
            break;
    }

    if (rc != 0)
        tapi_bt_adapter_free(adapter);

    return rc;
}

/** Set a boolean adapter property. */
static te_errno
bt_set_flag(tapi_job_factory_t *factory, tapi_bt_backend backend,
            unsigned int feature, const char *what, bool on, int timeout_ms)
{
    tapi_bt_backend actual;
    te_string out = TE_STRING_INIT;
    int code = 0;
    te_errno rc;

    rc = bt_backend(factory, backend, timeout_ms, &actual);
    if (rc != 0)
        return rc;

    if (!tapi_bt_supports(actual, feature))
    {
        ERROR("%s cannot be set through %s", what,
              tapi_bt_backend2str(actual));
        return TE_RC(TE_TAPI, TE_EOPNOTSUPP);
    }

    switch (actual)
    {
        case TAPI_BT_BLUEZ:
        case TAPI_BT_HCI:
        {
            const char *argv[2] = { what, on ? "on" : "off" };

            rc = bt_run(factory, "btmgmt", argv, TE_ARRAY_LEN(argv),
                        timeout_ms, &out, &code);
            break;
        }

        case TAPI_BT_MACOS:
        {
            /*
             * blueutil, not system_profiler: system_profiler reads and
             * nothing else. A macOS agent that has not been given
             * blueutil can be asked about its Bluetooth and cannot be
             * told anything.
             */
            const char *argv[2] = {
                strcmp(what, "power") == 0 ? "--power" : "--discoverable",
                on ? "1" : "0",
            };

            rc = bt_run(factory, "blueutil", argv, TE_ARRAY_LEN(argv),
                        timeout_ms, &out, &code);
            break;
        }

        case TAPI_BT_WINDOWS:
        {
            te_string script = TE_STRING_INIT;

            /*
             * Disabling the PnP device rather than the radio: the
             * WinRT radio API needs an async call and a consent
             * prompt, and Disable-PnpDevice is what a headless line
             * can do. It needs administrator rights and it is a
             * bigger hammer - the device goes away rather than the
             * radio going quiet - which is said here because a test
             * that turns it off has to turn it back on.
             */
            te_string_append(&script,
                "$d = Get-PnpDevice -Class Bluetooth -ErrorAction "
                "SilentlyContinue | Where-Object { $_.InstanceId -like "
                "'USB*' -or $_.InstanceId -like 'PCI*' }; "
                "if ($d) { %s-PnpDevice -InstanceId $d[0].InstanceId "
                "-Confirm:$false }",
                on ? "Enable" : "Disable");

            rc = tapi_bt_powershell(factory, script.ptr, timeout_ms, &out,
                                    &code);
            te_string_free(&script);
            break;
        }

        default:
            rc = TE_RC(TE_TAPI, TE_EINVAL);
            break;
    }

    if (rc == 0 && code != 0)
    {
        ERROR("Setting %s %s failed (status %d): %s", what,
              on ? "on" : "off", code, te_string_value(&out));
        rc = TE_RC(TE_TAPI, TE_ESHCMD);
    }

    te_string_free(&out);

    return rc;
}

/* See description in tapi_bt.h */
te_errno
tapi_bt_power_set(tapi_job_factory_t *factory, tapi_bt_backend backend,
                  bool on, int timeout_ms)
{
    return bt_set_flag(factory, backend, TAPI_BT_FEAT_POWER, "power", on,
                       timeout_ms);
}

/* See description in tapi_bt.h */
te_errno
tapi_bt_discoverable_set(tapi_job_factory_t *factory,
                         tapi_bt_backend backend, bool on, int timeout_ms)
{
    return bt_set_flag(factory, backend, TAPI_BT_FEAT_DISCOVERABLE,
                       "discov", on, timeout_ms);
}

/* See description in tapi_bt.h */
void
tapi_bt_adapter_log(const tapi_bt_adapter *adapter)
{
    RING("Adapter %s (%s) through %s",
         adapter->address[0] != '\0' ? adapter->address : "?",
         adapter->identifier != NULL ? adapter->identifier : "?",
         tapi_bt_backend2str(adapter->backend));
    RING("  name: %s", adapter->name != NULL ? adapter->name : "?");
    RING("  powered: %s, discoverable: %s, pairable: %s",
         adapter->powered ? "yes" : "no",
         adapter->discoverable ? "yes" : "no",
         adapter->pairable ? "yes" : "no");
    if (adapter->detail != NULL)
        RING("  %s", adapter->detail);
}

/* See description in tapi_bt.h */
void
tapi_bt_adapter_free(tapi_bt_adapter *adapter)
{
    free(adapter->name);
    free(adapter->identifier);
    free(adapter->detail);
    memset(adapter, 0, sizeof(*adapter));
}
