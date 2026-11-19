# Share a RAK2287 between MeshCore and Meshtastic

The Linux radio broker owns the SPI device once and keeps both SX1250 packet
receivers armed. MeshCore and Meshtastic run in separate processes. Each gets
its own socket and its own receive profile.

| Chain | Socket | Profile |
| --- | --- | --- |
| RF0 | `meshtastic.sock` | Paris Medium Fast, 869.300 MHz, 250 kHz, SF9, CR 4/5, sync 0x2b |
| RF1 | `meshcore.sock` | MeshCore EU Narrow, 869.618 MHz, 62.5 kHz, SF8, CR 4/8, sync 0x12 |

The Paris settings come from the [Mesh Île-de-France configuration guide](https://wiki.mesh-idf.fr/fr/Meshtastic/Configurer)
and the [Meshtastic modem presets](https://meshtastic.org/docs/overview/radio-settings/).

## Listen to both networks

Install the build tools from the project overview, initialize the submodules,
and build the broker and MeshCore observer.

```sh
git submodule update --init --recursive
make -j2
```

Stop the current SPI owner and reset the idle board using its reset helper.
On a Blackspot, run `./reset_lgw.sh start` from the gateway tools directory.
[Board setup and reset](https://github.com/guilhem/rak2287-radio/tree/main/driver)
Use the same Linux account and `XDG_RUNTIME_DIR` for the broker and its clients.

```sh
socket_dir="$XDG_RUNTIME_DIR/rak-radio"
install -d -m 0700 "$socket_dir"
build/radio-broker "$socket_dir" /dev/spidev0.0
```

In the second terminal, connect the observers.

```sh
export MESHCORE_DATA_DIR="$XDG_RUNTIME_DIR/rak-observer"
install -d -m 0700 "$MESHCORE_DATA_DIR"
python3 -u integration/watch_radio_broker.py "$XDG_RUNTIME_DIR/rak-radio" |
  python3 -u deps/rak2287-radio/integration/observe_profiles.py --meshcore meshcore --meshtastic meshtastic \
    --monitor build/meshcore-rx-monitor
```

The broker prints `RADIO_BROKER_READY` once both receivers are armed.
The observer reports MeshCore routing decisions and Meshtastic packet
headers. The broker starts with transmission disabled. Stop it and reset
the board before starting the normal receiver or packet forwarder.

## Simulate shared transmission

`make check` runs the broker with a fake radio and two Unix socket clients.
The same suite links the real driver to a simulated HAL to verify that an RF0
transmission with an explicit profile can be followed by reception on both
original profiles.

Each client can have one queued transmission. The broker takes requests in
rotation, defers while either radio is receiving, sends through RF0, then
rearms both receivers. It spaces transmissions using the estimated airtime
of the last send. This pacing restarts with the broker. Requests that
cannot start within two seconds return busy. The radio has not started
transmitting and the caller can retry.
When the outcome is uncertain, the caller discards the request.
Both adapters allow up to 12 seconds for a reply, including time that may
be spent finishing another client's transmission.

The broker's transmit opt-in is intended for simulation and controlled RF
bench qualification. At 869.300 MHz the nominal 250 kHz channel extends
across French bands reserved for alarms. Field testing for the Paris
profile uses receive-only mode. The RF0 path still needs conducted power
and spectrum measurements. [French frequency conditions](https://www.legifrance.gouv.fr/jorf/article_jo/JORFARTI000048119817)

## Connect the mesh programs

For a simulation or a controlled RF bench with a TX-enabled broker,
build with `make` and run
`build/meshcore-radio-repeater --enable-transmit --socket "$XDG_RUNTIME_DIR/rak-radio/meshcore.sock"`.
Set `MESHCORE_DATA_DIR` to the repeater's private state directory. The
repeater exits if the broker rejects a relay because TX is disabled.
Use the observer for receive-only MeshCore operation.

The Meshtastic Portduino adapter opens `meshtastic.sock`. Stop the
observer before starting these programs because it occupies both sockets.
[Build the Meshtastic adapter](meshtastic/PORTDUINO.md)

Both adapters use local Unix `SOCK_SEQPACKET` connections. The broker accepts
one connection per profile. A receive record has byte `R`, RSSI and SNR as
signed big-endian 16-bit values in quarter dB, and the raw LoRa frame. A
transmit request has byte `T` followed by 1–255 raw frame bytes. The broker
replies with `A` and one status byte after accepting the request.

| Status | Meaning |
| --- | --- |
| 0 | TxDone and both receivers rearmed |
| 1 | Busy and no attempt to transmit |
| 2 | Transmission disabled |
| 3 | Error with no delivery guarantee |

The broker drops a receive record if a client stops reading and its socket
is full. When an unstarted request is pending, a disconnected client loses
that request. A client that cannot read the outcome of an in-flight
transmission treats it as uncertain.

## Hardware trial

On black1, a four-minute run in receive-only mode delivered 16 CRC-valid
frames to the Meshtastic Paris socket and four to the MeshCore EU Narrow
socket. The observer parsed six unique Meshtastic sender-and-packet-ID
pairs and reported the MeshCore routing decisions. In a separate check,
both hardware-backed sockets returned status 2 in receive-only mode.
The normal RF1 listener recovered the SPI device after the trials.

A separate 200-second board-level trial tuned both receivers to the same
EU Narrow transmission. The radios reported 23 matching CRC-valid frames,
with both receive interrupts latched before either FIFO was read.

The native MeshCore repeater and Meshtastic daemon also sent through a
single simulated broker. The MeshCore frame carried the repeater hop on
the 62.5 kHz profile. The Meshtastic frame used the 250 kHz Paris profile.
