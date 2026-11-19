"""Read both broker sockets and print profile-tagged reception records."""

import argparse
from pathlib import Path
import selectors
import socket
import struct
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("socket_directory", type=Path)
    args = parser.parse_args()

    with selectors.DefaultSelector() as selector:
        for profile in ("meshcore", "meshtastic"):
            peer = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            peer.connect(str(args.socket_directory / f"{profile}.sock"))
            selector.register(peer, selectors.EVENT_READ, profile)
        while True:
            for key, _ in selector.select():
                record = key.fileobj.recv(261)
                if not record:
                    return
                if record[0] != ord("R") or not 6 <= len(record) <= 260:
                    raise ValueError("Invalid broker receive record")
                rssi, snr = struct.unpack_from(">hh", record, 1)
                timestamp = time.monotonic_ns() // 1000
                print(f"RX\t{key.data}\t{timestamp}\t{rssi / 4}\t{snr / 4}\t{record[5:].hex()}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
