"""Test the real broker against a fake radio and two independent clients."""

from contextlib import contextmanager
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

BROKER = Path(sys.argv.pop(1))
REPEATER = Path(sys.argv.pop(1))


class BrokerTest(unittest.TestCase):
    @contextmanager
    def broker(self, *, tx=True, **env):
        with tempfile.TemporaryDirectory() as dirname:
            directory = Path(dirname)
            args = [BROKER, directory, "/fake-spi"]
            if tx:
                args.append("--enable-transmit")
            with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                  text=True, env={**os.environ, **env}) as process:
                self.assertIn("RADIO_BROKER_READY", process.stdout.readline())
                try:
                    yield process, directory
                finally:
                    process.terminate()
                    process.communicate(timeout=5)

    def peer(self, directory, profile):
        conn = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.addCleanup(conn.close)
        conn.settimeout(4)
        conn.connect(str(directory / f"{profile}.sock"))
        return conn

    def test_two_clients_share_one_transmit_queue_and_keep_receive_profiles(self):
        with self.broker(FAKE_RX="1") as (process, directory):
            meshcore = self.peer(directory, "meshcore")
            meshtastic = self.peer(directory, "meshtastic")
            meshcore.sendall(b"T\x11")
            meshtastic.sendall(b"T\x22")
            for conn in (meshtastic, meshcore):
                record = conn.recv(260)
                self.assertEqual(record[:5], struct.pack(">Bhh", ord("R"), -382, 17))
                self.assertEqual(conn.recv(260), b"A\x00")
            first = process.stdout.readline()
            second = process.stdout.readline()
            self.assertIn("hz=869300000 bw=250000 payload=22", first)
            self.assertIn("hz=869618000 bw=62500 payload=11", second)

    def test_receive_only_rejects_transmit(self):
        with self.broker(tx=False) as (process, directory):
            conn = self.peer(directory, "meshtastic")
            conn.sendall(b"T\x22")
            self.assertEqual(conn.recv(260), b"A\x02")
            process.terminate()
            self.assertNotIn("SET_TX", process.stdout.read())

    def test_expired_or_ambiguous_requests_are_not_duplicated(self):
        with self.broker(FAKE_ALWAYS_BUSY="1") as (process, directory):
            conn = self.peer(directory, "meshcore")
            conn.sendall(b"T\x11")
            self.assertEqual(conn.recv(260), b"A\x01")
            process.terminate()
            self.assertNotIn("SET_TX", process.stdout.read())
        with self.broker(FAKE_AMBIGUOUS="1") as (process, directory):
            conn = self.peer(directory, "meshcore")
            conn.sendall(b"T\x11")
            self.assertEqual(conn.recv(260), b"A\x03")
            process.terminate()
            self.assertEqual(process.stdout.read().count("SET_TX"), 1)

    def test_meshcore_repeater_uses_its_socket_for_real_forwarding(self):
        with self.broker(FAKE_RX="1") as (broker, directory):
            state = directory / "meshcore-state"
            state.mkdir(mode=0o700)
            args = [REPEATER, "--enable-transmit", "--socket", directory / "meshcore.sock"]
            with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                                  env={**os.environ, "MESHCORE_DATA_DIR": str(state)}) as repeater:
                try:
                    started = time.monotonic()
                    while time.monotonic() - started < 2 and not (state / "repeater.id").exists():
                        time.sleep(0.01)
                    self.assertTrue((state / "repeater.id").exists())
                    meshtastic = self.peer(directory, "meshtastic")
                    meshtastic.sendall(b"T\x22")
                    self.assertEqual(meshtastic.recv(260)[0], ord("R"))
                    self.assertEqual(meshtastic.recv(260), b"A\x00")
                    self.assertIn("bw=250000 payload=22", broker.stdout.readline())
                    relay = broker.stdout.readline().strip()
                    hop = (state / "repeater.id").read_bytes()[:2].hex()
                    self.assertIn(f"bw=62500 payload=1541{hop}aa01020304050607", relay)
                finally:
                    repeater.terminate()
                    _, stderr = repeater.communicate(timeout=5)
                self.assertEqual(repeater.returncode, 0, stderr)

    def test_meshcore_rejects_the_meshtastic_endpoint(self):
        with self.broker(tx=False) as (_, directory):
            state = directory / "meshcore-state"
            state.mkdir(mode=0o700)
            args = [REPEATER, "--enable-transmit", "--socket", directory / "meshtastic.sock"]
            result = subprocess.run(args, capture_output=True, text=True, timeout=4,
                                    env={**os.environ, "MESHCORE_DATA_DIR": str(state)})
            self.assertEqual(result.returncode, 1)
            self.assertIn("broker connect", result.stderr)


if __name__ == "__main__":
    unittest.main()
