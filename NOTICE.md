# Source and license notices

The broker, tests, and MeshCore socket integration in this repository use
the [BSD 3-Clause License](LICENSE). This integration builds on the native
repeater in the pinned [rak2287-radio dependency](https://github.com/guilhem/rak2287-radio).
Its notices cover the MeshCore runtime, Semtech driver and HAL, and
cryptography dependencies.

The separate [Meshtastic Portduino adapter](integration/meshtastic/PORTDUINO.md)
uses [GPL-3.0-only](integration/meshtastic/COPYING) and builds with the
[pinned Meshtastic firmware](https://github.com/meshtastic/firmware/tree/647831b251fb2349a8ce85d78490874ccb3bffac).
