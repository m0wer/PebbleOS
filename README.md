# PebbleOS custom build

This repository tracks the [official PebbleOS source](https://github.com/coredevices/PebbleOS)
and carries a small patch stack for personal builds. Upstream documentation remains the best
reference for [building](https://pebbleos-core.readthedocs.io/en/latest/development/getting_started.html)
and [contributing](https://github.com/coredevices/PebbleOS/blob/main/CONTRIBUTING.md).

**[Download the latest firmware release](https://github.com/m0wer/PebbleOS/releases/latest)**

[PebbleApp](https://github.com/m0wer/PebbleApp) is the compatible installer and update client.

## Changes

- Retain fragmented sleep periods and Quiet Time context during sleep classification
  ([upstream PR #1742](https://github.com/coredevices/PebbleOS/pull/1742)).
- Record battery current in analytics heartbeats (`publish/battery-current-logging`).
- Allow notification dismissal with a double-flick gesture
  ([upstream issue #1595](https://github.com/coredevices/PebbleOS/issues/1595)).
- Expose raw voice recording sessions to applications
  ([upstream PR #1035](https://github.com/coredevices/PebbleOS/pull/1035)).
- Build Moddable host utilities without a graphical session (`publish/headless-moddable-tools`).
- Export sleep-classification diagnostics for analysis (`publish/sleep-diagnostics`).
- Add app-data backup support for [PebbleApp](https://github.com/m0wer/PebbleApp).

Branches named `publish/*` contain focused changes based directly on the upstream branch.

## Install

Download the PBZ for your exact watch and hardware revision from the
[latest release](https://github.com/m0wer/PebbleOS/releases/latest) or a
[CI prerelease](https://github.com/m0wer/PebbleOS/releases). Use the direct PBZ release or prerelease
assets for installation. GitHub Actions artifact ZIPs also contain debug files and are not recommended
for installation.

- Pebble 2 Duo: `PebbleOS-asterix.pbz`
- Pebble Time 2: `PebbleOS-obelix_dvt.pbz` or `PebbleOS-obelix_pvt.pbz`
- Pebble Round 2: `PebbleOS-getafix_evt.pbz`, `PebbleOS-getafix_dvt.pbz`, or
  `PebbleOS-getafix_dvt2.pbz`

The app checks the PBZ hardware revision before transferring it, so trying the candidates for your
product is safe. A mismatch reports both revisions, for example
`CORE_OBELIX_DVT != CORE_OBELIX_PVT`: the value on the right is the watch, so use
`PebbleOS-obelix_pvt.pbz`. Revisions are not interchangeable. Release assets are production builds;
`SHA256SUMS` contains their checksums.

Transfer the downloaded `.pbz` file to the phone paired with the watch. In the Pebble app, enable
**Settings > Show debug options**, open the watch in **Devices**, then choose
**Firmware Update Debug > Sideload FW**. See the
[firmware loading guide](docs/development/building_fw.md#loading-firmware-via-bluetooth) for other
builds and ADB installation.

A normal PBZ update through the app preserves watch settings and data. Factory reset, fast factory
reset, and full-device flashing or erase operations do not.

## License

PebbleOS remains licensed under [Apache License 2.0](LICENSE), with separately licensed third-party
components. Original patches authored by m0wer are also offered under the [MIT License](LICENSE-MIT)
to the extent that m0wer owns them. The MIT grant does not relicense upstream or third-party code.
