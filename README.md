# tsf-bluetooth

Bluetooth from a test suite, packaged as an external Test Environment
(TE) repository.

Library:

- `tapi_bt` — engine-side, built as a shared library: the adapter of a
  Test Agent, the devices around it, what the pairing between them is
  worth, and the HCI link itself — on Linux, macOS and Windows.

TE has no Bluetooth of its own. There is a PCI subclass constant in
`te_pci_ids.h` and nothing else, on any branch.

## Usage

Declare the repositories in an external libraries catalog and pass it
to `dispatcher.sh --external=external.yml`:

```yaml
repositories:
  - name: tsf_devtool
    url: https://github.com/interpretica-io/tsf-devtool.git
    ref: <tag>
    libs: [ tapi_devtool ]
  - name: tsf_cybersec
    url: https://github.com/interpretica-io/tsf-cybersec.git
    ref: <tag>
    libs: [ tapi_cybersec ]
  - name: tsf_bluetooth
    url: https://github.com/interpretica-io/tsf-bluetooth.git
    ref: <tag>
    libs: [ tapi_bt ]
```

Bind them in `builder.conf`:

```
TE_EXT_REPO_USE([tsf_devtool], [], [tapi_devtool])
TE_EXT_REPO_USE([tsf_cybersec], [], [tapi_cybersec])
TE_EXT_REPO_USE([tsf_bluetooth], [], [tapi_bt])
```

Then add `tapi_bt` to `te_libs` in the suite's `meson.build`.
Requires TE with `TE_EXT_REPO` support and an **RPC** job factory
(`ta_rpcprovider` on the agent).

```c
tapi_bt_adapter adapter;

if (!tapi_bt_available(factory, TAPI_BT_AUTO, 10000))
    TEST_SKIP("There is no Bluetooth adapter on the agent");

CHECK_RC(tapi_bt_adapter_get(factory, TAPI_BT_AUTO, 10000, &adapter));
RING("%s is %s", adapter.address, adapter.powered ? "on" : "off");
tapi_bt_adapter_free(&adapter);
```

## Four backends, and no pretence

There is no portable way to drive Bluetooth. Every system exposes a
different thing, and the differences are not spellings of one idea, so
the common core is only what all four genuinely do and the rest sits
behind `tapi_bt_supports()`.

| | BlueZ | HCI | macOS | Windows |
|---|---|---|---|---|
| adapter address, name, power | yes | yes | yes | yes |
| power on and off | yes | yes | yes | yes |
| discoverable | yes | yes | yes | no |
| scan for devices | yes | yes | yes | yes |
| known and connected devices | yes | no | yes | yes |
| pair, remove | yes | no | yes | no |
| connect, disconnect | yes | no | yes | no |
| RSSI | yes | yes | yes | no |
| HCI trace | yes | yes | no | no (ETW) |
| l2ping, SDP browse | yes | yes | no | no |

- **BlueZ** — Linux with `bluetoothd` running. `bluetoothctl` over
  D-Bus for devices and pairing, `btmgmt` for the adapter. The
  complete one.
- **HCI** — Linux without the daemon: `hciconfig`, `hcitool`, `btmgmt`
  talking to the kernel. No pairing and no device list — those live in
  the daemon — but it works on an embedded image that has no D-Bus.
- **macOS** — `blueutil` where it is installed, and `system_profiler`,
  which is always there, for everything that can be read rather than
  changed. A Mac without `blueutil` can be asked about its Bluetooth
  and cannot be told anything.
- **Windows** — PowerShell: the PnP device tree for what is there, the
  registry for the radio's own address. Windows does not give a
  program the HCI link at all.

Two things Windows genuinely cannot do, rather than has not been
taught: there is **no API that reports whether the adapter is
discoverable** — the Settings app owns that — and **pairing needs a
consent ceremony** that a headless PowerShell line cannot answer. Both
are absent from the table above rather than stubbed.

## Working out which one it is

`TAPI_BT_AUTO` asks the agent. It is worth knowing how, because the
obvious way does not work. Both of these were measured on BlueZ 5.72:

- **`hcitool dev` exits 0 with no adapter at all.** It prints
  `Devices:` and an empty list, so its exit status says nothing and
  the list has to be read.
- **`bluetoothctl` aborts when `bluetoothd` is not running** — not an
  error message and a status, a `SIGABRT` out of libdbus, arriving as
  exit code 134 or as a signal.

So detection goes by `btmgmt` and `hciconfig`, which fail honestly
(exit 1 with `Unable to open mgmt_socket` and `Can't open HCI socket`),
and only reaches for `bluetoothctl` once something has said an adapter
exists.

## Known devices and devices in range

Two different questions, kept apart because the answers mean different
things. `tapi_bt_known()` returns what the system has paired with or
seen before — reading it changes nothing and takes no time.
`tapi_bt_scan()` runs an inquiry, which takes seconds and puts the
radio on the air.

A paired device that is switched off is in the first and not the
second. A stranger's headset is in the second and not the first.
Asking one and reporting the other is the mistake the separation
exists to prevent.

Addresses are normalised on the way in. BlueZ writes
`20:15:82:EF:D2:5D`, `blueutil` writes `20-15-82-ef-d2-5d`, and
Windows hides one inside a PnP instance id; comparing them as they come
is a bug waiting for the first test that pairs on one system and checks
on another.

## What the posture is worth

`tapi_bt_audit()` reads the settings the system reports and turns them
into [tsf-cybersec](https://github.com/interpretica-io/tsf-cybersec)
findings.

| Finding | Severity | Read from |
|---|---|---|
| `bt.debug-keys` | critical | `debug-keys` in the settings |
| `bt.no-ssp` | high | classic pairing without Secure Simple Pairing |
| `bt.discoverable` | high / medium | answers an inquiry from strangers |
| `bt.no-secure-connections` | medium | classic without `secure-conn` |
| `bt.unexpected-bond` | medium | a bond the policy does not list |
| `bt.pairable` | low | accepts a bond it did not ask for |
| `bt.no-le-privacy` | low | LE without `privacy` |
| `bt.name-broadcast` | low | discoverable under a readable name |
| `bt.powered-off` | info | the radio is off, so nothing else was measured |
| `bt.not-assessed` | info | the system does not report how it pairs |
| `bt.bonds-not-assessed` | info | the system keeps no list of bonds |

`bt.debug-keys` is the one worth knowing about: the debug key is
published in the specification, so a link encrypted with it can be
decrypted by anyone within earshot. It exists so that a sniffer can be
pointed at a device on a bench, and it is exactly the setting that gets
left on.

The three `info` findings are there because most of this is read from
one line of `btmgmt` output, which only Linux produces. A silent report
from a system that could not be asked is worse than no report, so the
audit says which questions it did not get to.

## Recording the link

`tapi_bt_hci` runs `btmon` and keeps the capture on the agent. **Linux
only, and that is not an omission**: the Linux kernel offers a monitor
socket that hands a copy of every HCI packet to anything allowed to
listen. macOS keeps the link inside the system — PacketLogger sees it,
and PacketLogger is a separate download a test cannot assume. Windows
has an ETW trace, which is a different capture in a different format
(`.etl`), started with `netsh` or `logman` and converted before
anything can read it; `tapi_bt_hci_start()` says so rather than
producing something that looks like a capture and is not.

Starting it waits for two things, and the second is the one that
matters. Measured on BlueZ 5.72, `btmon` prints
`Bluetooth monitor ver 5.72` on standard output **whether or not it
managed to open anything**, writes the real complaint to standard
error, and exits 1. A readiness check that only looked for the banner
would report a running capture every time. The banner says it started;
still being alive a moment later says it worked.

## What was verified, and what was not

Held to the same standard as the rest of these repositories, which
here means being specific about an uneven result.

**macOS — verified live.** The whole path was run against a real
controller: `system_profiler SPBluetoothDataType -json` parsed for the
controller address, power state, discoverable state and chipset;
`blueutil --paired` and `--connected` walked as JSON arrays; eight
paired devices read back with their names, bond and connection state;
addresses normalised from `blueutil`'s lower-case dashed form. RSSI
came back only for the connected device, which is what `blueutil` 2.13
does.

**Linux — command shapes verified, behaviour not.** The output formats
were taken out of the BlueZ 5.72 binaries themselves rather than from
memory — `addr %s version %u manufacturer %u class 0x%02x%02x%02x`,
`current settings: %s`, `hci%u dev_found: %s type %s rssi %d`,
`BD Address: %s  ACL MTU:` — and the no-adapter behaviour of every tool
was measured, which is what the detection and the skip path rest on.
What could not be exercised is an adapter: the kernel this was written
on has no Bluetooth subsystem at all, so `AF_BLUETOOTH` does not exist
and not even a virtual controller can be attached.

**Windows — not verified.** Written from the WinRT and PowerShell
documentation with no Windows machine to run it on. The first suite to
use it should expect to correct something.

## Scope

- **It drives the agent's own adapter**, and scans for what is
  advertising near it. An inquiry is a normal Bluetooth operation and
  finds only what is willing to be found.
- **Nothing here pairs with a device the test did not name**, and
  nothing goes looking for one to attack.
- **Power, discoverability and pairing change the agent**, and this
  library does not put them back. Configurator would; a job does not.
  A test that turns a radio off is responsible for turning it on
  again, in a cleanup section that runs even when the test failed —
  and that is said on each of those functions, not only here.
