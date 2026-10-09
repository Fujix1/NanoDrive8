"""Host tests of the production C++ parser; pass the compiled bridge DLL path.
See tests/ndsif/README.md for the native Windows build command. No serial port is opened.
"""
import binascii
import ctypes as c
from pathlib import Path
import random
import struct
import sys
import unittest

DLL = c.CDLL(str(Path(sys.argv.pop(1)).resolve()))
DLL.feed.argtypes = [c.c_uint8, c.c_uint32]
DLL.output.argtypes = [c.POINTER(c.c_uint8), c.c_uint]


def encode(data):
    out = bytearray([0])
    pos, code = 0, 1
    for b in data:
        if not b:
            out[pos] = code
            pos, code = len(out), 1
            out.append(0)
        else:
            out.append(b)
            code += 1
            if code == 255:
                out[pos] = code
                pos, code = len(out), 1
                out.append(0)
    out[pos] = code
    return bytes(out)


def decode(data):
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        assert code and i + code <= len(data)
        out.extend(data[i+1:i+code])
        i += code
        if code != 255 and i < len(data):
            out.append(0)
    return bytes(out)


def packet(op, payload=b"", req=0x1234, version=1, length=None):
    raw = struct.pack("<2sBBHH", b"ND", version, op, req,
                      len(payload) if length is None else length) + payload
    raw += struct.pack("<H", binascii.crc_hqx(raw, 0xffff))
    return b"\0" + encode(raw) + b"\0"


class ProtocolTests(unittest.TestCase):
    def test_output_volume_range_completion_and_reset(self):
        for attenuation in range(97):
            self.send(packet(0x03, bytes([attenuation]), req=attenuation))
            self.assertEqual(DLL.volumeValue(1), attenuation)
            self.assertEqual(DLL.count(2), 0)
            self.reply(0x03, b'\x00', req=attenuation, chunk=1)
        self.assertEqual(DLL.volumeValue(0), 97)
        self.send(packet(0))
        self.reply(0, b'\x00')
        self.assertEqual(DLL.volumeValue(1), 96)

    def test_output_volume_rejection_crc_and_optional_callback(self):
        for payload in [b'', b'\x00\x01'] + [bytes([v]) for v in range(97, 256)]:
            self.send(packet(0x03, payload))
            self.reply(0x03, b'\x01')
        raw = bytearray(decode(packet(0x03, b'\x10')[1:-1]))
        raw[8] ^= 1
        self.send(b'\0' + encode(raw) + b'\0')
        self.assertEqual(self.drain(), b'')
        self.assertEqual(DLL.volumeValue(0), 0)
        DLL.noVolume()
        self.send(packet(0x03, b'\x00'))
        self.reply(0x03, b'\x01')
        self.assertEqual(DLL.volumeValue(0), 0)

    def test_audio_packets_are_one_way_and_crc_gated(self):
        for opcode in (0x58, 0x59, 0x5a):
            self.send(packet(opcode, bytes(range(256))))
            self.assertEqual(self.drain(), b'')
            self.assertEqual(DLL.audioValue(1), opcode)
            self.assertEqual(DLL.audioValue(2), 256)
        self.assertEqual(DLL.audioValue(0), 3)
        bad = bytearray(packet(0x58, b'abc'))
        bad[-3] ^= 0x80
        self.send(bad)
        self.assertEqual(DLL.audioValue(0), 3)

    def test_audio_status_explicit_query(self):
        self.send(packet(0x5b))
        result = self.drain()
        raw = decode(result[1:-1])
        self.assertEqual(raw[3], 0xdb)
        self.assertEqual(raw[8:-2], b'\x00' + bytes(range(40)))
        self.assertEqual(binascii.crc_hqx(raw[:-2], 0xffff), struct.unpack('<H', raw[-2:])[0])
        self.send(packet(0x5b, b'x'))
        raw = decode(self.drain()[1:-1])
        self.assertEqual(raw[8], 1)
        self.assertEqual(DLL.audioValue(0), 1)

    def setUp(self):
        DLL.init()

    def send(self, data, now=10):
        for b in data:
            DLL.feed(b, now)

    def drain(self, chunk=4096):
        result = bytearray()
        buffer = (c.c_uint8 * chunk)()
        while True:
            n = DLL.output(buffer, chunk)
            if not n:
                return bytes(result)
            result.extend(buffer[:n])

    def reply(self, op, payload, req=0x1234, chunk=4096):
        wire = self.drain(chunk)
        self.assertTrue(wire.startswith(b"\0") and wire.endswith(b"\0"))
        raw = decode(wire[1:-1])
        self.assertEqual(raw[:8], struct.pack("<2sBBHH", b"ND", 1, op | 128, req, len(payload)))
        self.assertEqual(raw[8:-2], payload)
        self.assertEqual(int.from_bytes(raw[-2:], "little"), binascii.crc_hqx(raw[:-2], 0xffff))

    def test_ping_fragmented_and_partial_transmit(self):
        data = bytes(range(32))
        wire = packet(1, data)
        for i, byte in enumerate(wire):
            DLL.feed(byte, i)
            if i != len(wire)-1:
                self.assertEqual(self.drain(), b"")
        self.reply(1, b"\0" + data, chunk=1)

    def test_info_and_reset_completion(self):
        self.send(packet(2))
        self.reply(2, b"\0\x0bNanoDrive 8\x051.0b8")
        self.send(packet(0))
        self.assertEqual(DLL.count(0), 1)
        self.assertEqual(DLL.count(2), 0)
        self.reply(0, b"\0")

    def test_maximum_batch_and_write_order(self):
        data = bytes(range(256))
        self.send(packet(0x54, data))
        self.assertEqual(DLL.count(1), 128)
        self.assertEqual(bytes(DLL.pair(i) for i in range(256)), data)
        self.assertEqual(DLL.count(2), 0)
        self.reply(0x54, b"\0")

    def test_invalid_arguments_have_no_side_effects(self):
        for op, payload in [(0,b"x"), (2,b"x"), (1,b"x"*33),
                            (0x54,b""), (0x54,b"abc"), (0x7f,b"")]:
            self.send(packet(op,payload))
            self.reply(op,b"\1")
        self.assertEqual(DLL.count(0),0)
        self.assertEqual(DLL.count(1),0)

    def test_bad_crc_header_length_and_version(self):
        raw = decode(packet(0x54,b"ab")[1:-1])
        for i in range(len(raw)):
            for bit in range(8):
                damaged = bytearray(raw)
                damaged[i] ^= 1 << bit
                self.send(b"\0" + encode(damaged) + b"\0")
                self.assertEqual(self.drain(), b"")
        for wire in [packet(0, version=2), packet(0, length=1), packet(0x54,b"x"*258),
                     packet(0x81), b"\0\x05x\0", b"\0"+b"x"*1000+b"\0"]:
            self.send(wire)
            self.assertEqual(self.drain(),b"")
        self.assertEqual(DLL.count(1),0)
        self.assertEqual(DLL.count(0),0)
        self.send(packet(1))
        self.reply(1,b"\0")

    def test_timeout_and_millis_wrap(self):
        for start in [100,0xfffffff0]:
            wire = packet(0)
            self.send(wire[:-1],start)
            DLL.expire(c.c_uint32(start+500))
            self.send(wire[-1:],(start+501)&0xffffffff)
            self.assertEqual(self.drain(),b"")
        self.assertEqual(DLL.count(0),0)
        self.send(packet(1))
        self.reply(1,b"\0")

    def test_transport_reset_discards_rx_and_tx(self):
        self.send(packet(0)[:-1])
        DLL.clear(1)
        self.send(b"tail\0")
        self.assertEqual(DLL.count(0),0)
        self.send(packet(1))
        DLL.clear(1)
        self.assertEqual(self.drain(),b"")
        self.send(packet(2))
        self.reply(2,b"\0\x0bNanoDrive 8\x051.0b8")

    def test_bursts_have_no_replies_and_do_not_stall_following_commands(self):
        # Many full bursts in a coalesced RX stream: no per-frame TX drain.
        wire = b"".join(packet(0x56, bytes(range(256)), i) for i in range(64))
        self.send(wire)
        self.assertEqual(self.drain(), b"")
        self.assertEqual(DLL.count(1), 64*128)
        self.assertEqual(DLL.count(2), 0)
        self.send(packet(0))
        self.assertEqual(DLL.count(0), 1)
        self.reply(0, b"\0")
        self.send(packet(1, b"after burst"))
        self.reply(1, b"\0after burst")

    def test_startup_ping_orders_initial_writes_without_reset(self):
        # Initial writes must finish before the startup PING reply is generated.
        self.send(b"".join(packet(0x56, bytes([0x20, i]), i) for i in range(16)))
        self.assertEqual(self.drain(), b"")
        self.send(packet(1))
        self.assertEqual(DLL.count(1), 16)
        self.assertEqual(DLL.count(0), 0)
        self.reply(1, b"\0")
        self.send(packet(0x56, bytes([0x08, 0x78])))
        self.assertEqual(DLL.count(1), 17)
        self.assertEqual(self.drain(), b"")

    def test_malformed_bursts_are_silent_without_side_effects(self):
        for payload in [b"", b"x", b"xxx", b"x"*258]:
            self.send(packet(0x56, payload))
            self.assertEqual(self.drain(), b"")
        self.assertEqual(DLL.count(1), 0)
        self.send(packet(1))
        self.reply(1, b"\0")

    def test_clock_is_one_way_and_preserves_full_hz(self):
        for chip, hz in [(5, 4000000), (5, 3579545), (14, 8000000), (255, 0xffffffff)]:
            self.send(packet(0x57, struct.pack("<BI", chip, hz)))
            self.assertEqual(self.drain(), b"")
            self.assertEqual(DLL.clockValue(1), chip)
            self.assertEqual(c.c_uint32(DLL.clockValue(2)).value, hz)
        self.assertEqual(DLL.clockValue(0), 4)
        self.assertEqual(DLL.count(2), 0)
        self.send(packet(0x56, b"\x20\xc7"))
        self.send(packet(1))
        self.assertEqual(DLL.count(1), 1)
        self.reply(1, b"\0")

    def test_invalid_clock_payload_is_silent_without_side_effects(self):
        for payload in [b"", b"\5", b"abcd", b"abcdef", struct.pack("<BI", 5, 0)]:
            self.send(packet(0x57, payload))
            self.assertEqual(self.drain(), b"")
        raw = bytearray(decode(packet(0x57, struct.pack("<BI", 5, 4000000))[1:-1]))
        raw[8] ^= 1  # Corrupted chip ID must fail CRC before callback.
        self.send(b"\0" + encode(raw) + b"\0")
        self.assertEqual(DLL.clockValue(0), 0)
        self.assertEqual(self.drain(), b"")
        self.send(packet(1))
        self.reply(1, b"\0")

    def test_random_batches_and_echo(self):
        rng = random.Random(2151)
        for _ in range(1000):
            DLL.init()
            op = rng.choice([1,0x54])
            size = rng.randrange(33) if op == 1 else rng.randrange(1,129)*2
            data = bytes(rng.randrange(256) for _ in range(size))
            req = rng.randrange(65536)
            self.send(packet(op,data,req))
            self.reply(op,b"\0" + (data if op==1 else b""),req,chunk=rng.randrange(1,20))
            if op==0x54:
                self.assertEqual(bytes(DLL.pair(i) for i in range(size)),data)


if __name__ == "__main__":
    unittest.main()
