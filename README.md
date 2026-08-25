# PebbleOS custom build

This repository tracks the [official PebbleOS source](https://github.com/coredevices/PebbleOS)
and carries a small patch stack for personal builds. Upstream documentation remains the best
reference for [building](https://pebbleos-core.readthedocs.io/en/latest/development/getting_started.html)
and [contributing](https://github.com/coredevices/PebbleOS/blob/main/CONTRIBUTING.md).

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

Branches named `publish/*` contain focused changes based directly on the upstream branch.

## Install

Open the latest successful **Build Firmware** workflow run and download the release artifact for
your watch. `qa-firmware-obelix_pvt` and `qa-firmware-getafix_dvt2` contain dual-slot release PBZ
files suitable for Bluetooth sideloading on those exact hardware revisions. Do not install a PBZ
built for a different board.

Extract the artifact and transfer its `.pbz` file to the phone paired with the watch. In the Pebble
app, enable **Settings > Show debug options**, open the watch in **Devices**, then choose
**Firmware Update Debug > Sideload FW**. See the
[firmware loading guide](docs/development/building_fw.md#loading-firmware-via-bluetooth) for other
builds and ADB installation.

## License

PebbleOS remains licensed under [Apache License 2.0](LICENSE), with separately licensed third-party
components. Original patches authored by m0wer are also offered under the [MIT License](LICENSE-MIT)
to the extent that m0wer owns them. The MIT grant does not relicense upstream or third-party code.
