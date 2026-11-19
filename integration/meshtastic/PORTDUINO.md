# Meshtasticd socket radio

This Linux transport connects the native Meshtastic router to the RAK2287
broker. The broker owns SPI and LoRa GPIOs. The daemon owns its identity,
node database, channels, packet encryption, and routing decisions.

## Build

Start from the root of this repository. Use the Linux dependencies from the
[Meshtastic Linux build guide](https://meshtastic.org/docs/development/linux/),
including a C++20 compiler, PlatformIO Core, and the development packages for
libgpiod 1.x, YAML-CPP, Bluetooth, libusb, libuv, JSONCPP, I²C, and OpenSSL.
Keep the firmware checkout and PlatformIO build cache on disk with space for
the native dependencies.

```sh
adapter="$PWD/integration/meshtastic"
git clone https://github.com/meshtastic/firmware.git meshtastic-firmware
git -C meshtastic-firmware checkout --detach 647831b251fb2349a8ce85d78490874ccb3bffac
git -C meshtastic-firmware submodule update --init --recursive
git -C meshtastic-firmware apply --check "$adapter/portduino.patch"
git -C meshtastic-firmware apply "$adapter/portduino.patch"
cp "$adapter/UnixRadio.cpp" meshtastic-firmware/src/platform/portduino/
cd meshtastic-firmware
platformio run -e native
```

The executable is `.pio/build/native/meshtasticd`. These adapter files build
against the pinned Meshtastic firmware and follow its
[GPL-3.0 license](COPYING).

GCC 15 may report that `isfinite` is undeclared in the pinned firmware's
`src/modules/Telemetry/Sensor/BME680Sensor.cpp`. In the separate firmware
checkout, replace `<math.h>` with `<cmath>` and qualify the two
`isfinite(lastGasOhms)` calls as `std::isfinite(lastGasOhms)`. The
Portduino patch covers only the radio backend.

## Connect to the broker

Run the broker and Meshtasticd under an account that can access the
`meshtastic.sock` endpoint. Use a private directory for the socket and a
separate persistent directory for the daemon's preferences and keys.

```sh
umask 077
mkdir -p ./meshtastic-state
printf 'Lora:\n  Module: sim\n' > ./meshtastic-socket.yaml
MESHTASTIC_RADIO_SOCKET=/run/rak2287/meshtastic.sock \
  .pio/build/native/meshtasticd -c ./meshtastic-socket.yaml -d ./meshtastic-state
```

Replace the socket path with the absolute path created by the broker, if
different. The explicit environment variable selects the socket transport.
The YAML supplies headless daemon configuration.

From a Meshtastic client connected to this daemon, set the same radio profile
as the broker. The defaults for this adapter are Paris Medium Fast at 869.300
MHz with 250 kHz bandwidth, SF9, CR4/5, preamble length 16, and sync word
0x2b. For example, using the Meshtastic Python CLI on the local machine:

```sh
meshtastic --host localhost --set lora.region EU_868 \
  --set lora.use_preset true --set lora.modem_preset MEDIUM_FAST \
  --set lora.override_frequency 869.3 --set lora.tx_enabled false
```

The adapter logs a profile mismatch and waits for the expected configuration.
Changing the daemon's provisioning does not retune the broker. When both
agree, look for `RAK2287 packet socket connected` in the native daemon log.
The daemon handles received traffic according to its normal role and
configured channel keys. Keep the Paris profile receive-only on the air.

## Socket contract

The Linux Unix-domain socket uses `SOCK_SEQPACKET`. Each datagram has one
type byte followed by its fields.

| Type | Direction | Fields |
| --- | --- | --- |
| `R` | Broker to daemon | Signed quarter-dBm RSSI and signed quarter-dB SNR, both big-endian 16-bit, followed by 1–255 raw LoRa bytes |
| `T` | Daemon to broker | 1–255 raw LoRa bytes in Meshtastic wire format |
| `A` | Broker to daemon | One status byte, 0 sent, 1 busy and unsent, 2 TX disabled, 3 error or unknown transmission outcome |

The adapter keeps one submitted TX awaiting acknowledgement and leaves
ordinary mesh backoff and routing in the daemon. The broker arbitrates
access to the physical radio and can defer a request before starting TX.
The adapter retries a request after an explicit busy-and-unsent reply up to
ten times, with a 500 ms delay. Local socket backpressure can also retry a
write that did not reach the broker. A dropped connection, malformed reply,
or acknowledgement timeout discards the outstanding request and reconnects
without replaying it.

## Validation

On Linux, a build of the pinned firmware passed a local test with two actual
Meshtastic daemons. The sender retried a byte-identical frame after a busy
reply. The receiver decrypted the frame and exposed its RSSI and SNR through
the native TCP API. The test also exercised the no-LoRa destination guard
and explicit `-s` simulation mode with a broker socket in the environment.
A separate run connected the native Meshtastic daemon and the MeshCore
repeater to the same simulated RAK2287 broker. Both selected their own
transmit profile.

The validation environment used Nix GCC 15. The temporary firmware
checkout qualified two telemetry `isfinite` calls as `std::isfinite`.
The remaining compilation commands omitted the explicit host
`/usr/include` search path to avoid mixing host and Nix libc headers,
then the native objects were linked with the Nix libraries.
