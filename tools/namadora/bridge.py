"""NDSIF bridge server; requires pyserial."""
import argparse
import binascii
import os
from fractions import Fraction
from pathlib import Path
import re
import select
import socket
import struct
import subprocess
import sys
import threading
import time
import xml.etree.ElementTree as ET

import serial

LISTEN_PORT = 32151


def cobs_encode(data):
    out = bytearray(b'\0')
    code_pos, code = 0, 1
    for value in data:
        if value == 0:
            out[code_pos] = code
            code_pos, code = len(out), 1
            out.append(0)
        else:
            out.append(value)
            code += 1
            if code == 255:
                out[code_pos] = code
                code_pos, code = len(out), 1
                out.append(0)
    out[code_pos] = code
    return bytes(out)


def cobs_decode(data):
    out, i = bytearray(), 0
    while i < len(data):
        code = data[i]
        if not code or i + code > len(data):
            raise ValueError('invalid COBS')
        i += 1
        out.extend(data[i:i + code - 1])
        i += code - 1
        if code < 255 and i < len(data):
            out.append(0)
    return bytes(out)


def frame(command, payload=b'', transaction=0, reply=False):
    raw = struct.pack('<2sBBBHH', b'ND', 1, int(reply), command, transaction, len(payload)) + payload
    raw += struct.pack('<H', binascii.crc_hqx(raw, 0xffff))
    return b'\0' + cobs_encode(raw) + b'\0'


class NDSIF:
    def __init__(self, port, device='nd8'):
        self.device = device
        self.serial = serial.Serial(port, 1500000 if device == 'nd6' else 115200,
                                    timeout=0.005 if device == 'nd6' else 0.05, write_timeout=2)
        self.transaction = 0
        self.rx = bytearray()
        self.volume_supported = False
        self.wake_recoveries = 0

    def send(self, command, payload=b''):
        packet = frame(command, payload)
        if self.serial.write(packet) != len(packet):
            raise IOError('incomplete serial write')

    def send_oneway(self, messages):
        """Write complete burst frames without PING or status round trips."""
        batch = bytearray()
        for command, payload in messages:
            packet = frame(command, payload)
            if batch and len(batch) + len(packet) > 2048:
                if self.serial.write(batch) != len(batch):
                    raise IOError('incomplete serial burst write')
                batch.clear()
            batch.extend(packet)
        if batch and self.serial.write(batch) != len(batch):
            raise IOError('incomplete serial burst write')

    def send_many(self, messages):
        # Coalesce complete NDSIF frames to avoid one OS/USB write per FM
        # event. Bound each in-flight window to the ND6 host guideline and use
        # PING as a barrier before handing off the next window.
        batch = bytearray()
        for command, payload in messages:
            packet = frame(command, payload)
            if batch and len(batch) + len(packet) > 2048:
                if self.serial.write(batch) != len(batch):
                    raise IOError('incomplete serial write')
                self.request(1)
                batch.clear()
            batch.extend(packet)
        if batch and self.serial.write(batch) != len(batch):
            raise IOError('incomplete serial write')

    def request(self, command, payload=b'', timeout=4):
        self.transaction = (self.transaction % 65535) + 1
        expected_transaction = self.transaction
        packet = frame(command, payload, expected_transaction, True)
        if self.serial.write(packet) != len(packet):
            raise IOError('incomplete serial write')
        deadline = time.perf_counter() + (min(timeout, 0.1) if self.device == 'nd6' and command == 0x1c else timeout)
        wake_at = time.perf_counter() + 0.01
        wake_count = 0
        while time.perf_counter() < deadline:
            # Reading 512 bytes with a timeout waits for the whole buffer,
            # introducing tens of milliseconds of latency into each status.
            count = max(1, min(512, getattr(self.serial, 'in_waiting', 0)))
            received = self.serial.read(count)
            self.rx.extend(received)
            if received:
                wake_at = time.perf_counter() + 0.01
            while b'\0' in self.rx:
                part, _, remaining = self.rx.partition(b'\0')
                self.rx = bytearray(remaining)
                if not part:
                    continue
                raw = cobs_decode(part)
                if len(raw) < 11 or binascii.crc_hqx(raw[:-2], 0xffff) != int.from_bytes(raw[-2:], 'little'):
                    raise ValueError('invalid response CRC or length')
                magic, version, kind, cmd, tx, length = struct.unpack('<2sBBBHH', raw[:9])
                if magic != b'ND' or version != 1 or kind != 2 or len(raw) != 11 + length:
                    raise ValueError('invalid response header')
                if cmd == command and tx == expected_transaction:
                    result = raw[9:-2]
                    if not result or result[0] != 0:
                        raise RuntimeError(f'NDSIF rejected command {command:02x}: {result.hex()}')
                    if wake_count:
                        self.wake_recoveries += 1
                        if self.wake_recoveries == 1:
                            print('ND6: recovered a delayed USB response using a PING', flush=True)
                    return result
            if self.device == 'nd6' and wake_count < 4 and time.perf_counter() >= wake_at:
                # Some HWCDC transfers stop after a response fragment. A PING
                # creates another reply, allowing the queued USB packet to drain.
                # Never resend audio data or the original control request.
                self.transaction = (self.transaction % 65535) + 1
                probe = frame(1, b'', self.transaction, True)
                if self.serial.write(probe) != len(probe):
                    raise IOError('incomplete NDSIF PING write')
                wake_count += 1
                wake_at = time.perf_counter() + 0.01
        raise TimeoutError(f'NDSIF command {command:02x} timed out (response fragment: {len(self.rx)} bytes, PINGs: {wake_count})')

    def setup(self, live=False, ordered=False):
        info = self.request(2)
        name_len = info[1]
        name = info[2:2 + name_len].decode('utf-8', 'replace')
        fw_len_pos = 2 + name_len
        fw = info[fw_len_pos + 1:fw_len_pos + 1 + info[fw_len_pos]].decode('utf-8', 'replace')
        print(f'NDSIF: {name}, firmware {fw}', flush=True)
        entries, offset, seen = [], 0, set()
        common_features = common_max_payload = None
        while offset != 0xffff:
            if offset in seen:
                raise ValueError('capability pagination loop')
            seen.add(offset)
            result = self.request(5, struct.pack('<H', offset))
            if len(result) < 14 or len(result) != 14 + 9 * result[13]:
                raise ValueError('invalid capability response')
            max_payload = int.from_bytes(result[1:3], 'little')
            if max_payload < 4:
                raise RuntimeError('NDSIF payload limit too small for register writes')
            features = int.from_bytes(result[9:11], 'little')
            if common_features is not None and (features != common_features or max_payload != common_max_payload):
                raise ValueError('inconsistent control capabilities')
            common_features, common_max_payload = features, max_payload
            self.volume_supported = bool(features & 1)
            entries.extend(result[i:i + 9] for i in range(14, len(result), 9))
            offset = int.from_bytes(result[11:13], 'little')
        if live and not ordered and not (common_features & 4):
            raise RuntimeError('ND6 firmware does not support NDSIF live DAC profile 2; update the firmware')
        if ordered and not (common_features & 8):
            raise RuntimeError('ND6 firmware does not support ordered live YM profile 3; update the firmware')
        chip_id = 0x52 if self.device == 'nd6' else 0x54
        clock_hz = 7670454 if self.device == 'nd6' else 4000000
        targets = [(0x52, 0), (0x53, 1)] if self.device == 'nd6' else [(0x54, 0)]
        chips = []
        for write_id, page in targets:
            matches = [e for e in entries if e[:3] == bytes((write_id, 0, page))]
            if len(matches) != 1 or not matches[0][3] & 1:
                raise RuntimeError(f'write {write_id:02x} instance 0/page {page} burst unavailable')
            chips.append(matches[0])
        chip = chips[0]
        self.chip_clock = chip
        if any(c[4:9] != chip[4:9] for c in chips[1:]):
            raise RuntimeError('YM2612 pages have inconsistent clock capabilities')
        self.live_max_records = 0
        if live and not ordered:
            offset, seen, targets_seen, found, signature = 0, set(), set(), False, None
            while offset != 0xffff:
                if offset in seen:
                    raise ValueError('live DAC capability pagination loop')
                seen.add(offset)
                cap = self.request(0x1e, struct.pack('<H', offset))
                if len(cap) < 15:
                    raise ValueError('invalid live DAC capability response')
                status, profile, revision, rate, capacity, maximum, next_offset, count = struct.unpack_from('<BBB I I B H B', cap)
                if (status != 0 or profile != 2 or revision != 1 or rate != 44100 or not capacity
                        or not 1 <= maximum <= min(capacity, 50, (max_payload - 3) // 5)
                        or len(cap) != 15 + count * 2):
                    raise RuntimeError('unsupported or invalid live DAC capabilities')
                page_signature = (profile, revision, rate, capacity, maximum)
                if signature is not None and signature != page_signature:
                    raise ValueError('inconsistent live DAC capabilities')
                signature = page_signature
                self.live_max_records = maximum
                for pos in range(15, len(cap), 2):
                    target = cap[pos:pos + 2]
                    if target in targets_seen:
                        raise ValueError('duplicate live DAC capability')
                    targets_seen.add(target)
                    if target == b'\x52\x00':
                        found = True
                if next_offset != 0xffff and next_offset <= offset:
                    raise ValueError('invalid live DAC capability offset')
                offset = next_offset
            if not found:
                raise RuntimeError('YM2612 instance 0 live DAC unavailable')
        if ordered:
            result = self.request(0x21)
            if len(result) != 10:
                raise ValueError('invalid ordered live capability response')
            status, profile, revision, rate, capacity, maximum = struct.unpack('<BBB I H B', result)
            if (status != 0 or profile != 3 or revision not in (4, 5) or rate != 44100 or not capacity
                    or not 1 <= maximum <= min(capacity, 100, (max_payload - 5) // 2)):
                raise RuntimeError('unsupported ordered live YM capabilities')
            self.ordered_psg = revision >= 5
            if not self.ordered_psg:
                print('ND6: PSG disabled; ordered profile revision 5 firmware required', flush=True)
            if self.ordered_psg:
                psg = [e for e in entries if e[:3] == b"\x50\x00\x00"]
                if len(psg) != 1 or not psg[0][3] & 1:
                    raise RuntimeError("ordered PSG capability unavailable")
                self.psg_clock = psg[0]
            self.ordered_max_records = maximum
            self.ordered_capacity = capacity
        if self.device == 'nd6':
            # Clear any previous timed-DAC session before clock or immediate writes.
            self.request(0)
        if chip[4] == 2 and chip[3] & 4:
            self.request(4, struct.pack('<BBI', chip_id, 0, clock_hz))
        elif chip[4] != 1 or int.from_bytes(chip[5:9], 'little') != clock_hz:
            raise RuntimeError(f'chip clock is unknown or incompatible with {clock_hz} Hz')
        if ordered and self.ordered_psg:
            self.set_psg_clock(3579545)
        self.max_pairs = min(127, (max_payload - 2) // 2)
        self.max_payload = max_payload
        if self.volume_supported:
            self.request(3, b'\x00')
        else:
            print('Warning: output volume control unavailable; pause cannot mute the device', flush=True)
        if live and not ordered:
            print('ND6: PSG disabled; use --ordered with revision 5 firmware', flush=True)
        mode = 'ordered live YM' if ordered else ('live DAC' if live else 'register burst')
        print(f'{self.device.upper()} {mode} and {clock_hz} Hz clock ready', flush=True)

    def set_psg_clock(self, hz):
        chip = self.psg_clock
        if chip[4] == 2 and chip[3] & 4:
            self.request(4, struct.pack('<BBI', 0x50, 0, hz))
        elif chip[4] != 1 or int.from_bytes(chip[5:9], 'little') != hz:
            raise RuntimeError(f'PSG clock is incompatible with {hz} Hz')

    def writes(self, pairs, page=0):
        write_id = (0x52 + page) if self.device == 'nd6' else 0x54
        if page not in ((0, 1) if self.device == 'nd6' else (0,)):
            raise ValueError('unsupported register page')
        for i in range(0, len(pairs), self.max_pairs * 2):
            self.send(write_id, bytes((0, page)) + pairs[i:i + self.max_pairs * 2])


def receive(conn, size, stop_event=None):
    while True:
        try:
            return conn.recv(size)
        except socket.timeout:
            if stop_event is not None and stop_event.is_set():
                return b''


def live_connection(nd, conn, batch_ms=2.0, dac_wait_scale=Fraction(1), batch_events=50, *, stop_event=None):
    pending = bytearray()
    previous_dac_tick = None
    scale = Fraction(str(dac_wait_scale))
    if not 0 < scale <= 4:
        raise ValueError('DAC wait scale must be greater than 0 and at most 4')
    if not 1 <= batch_events <= 256:
        raise ValueError('batch event limit must be between 1 and 256')
    remainder = 0
    count = 0
    dac_count = 0
    while True:
        batch_bytes = 12 * batch_events
        data = receive(conn, batch_bytes, stop_event)
        if not data:
            return count, dac_count
        # Gather a small amount of input before encoding NDSIF frames. The
        # deadline starts with the first received byte, not the latest one.
        # Control and FM events retain their order in the resulting batch.
        if batch_ms > 0:
            parts = [data]
            size = len(data)
            deadline = time.perf_counter() + batch_ms / 1000.0
            while size < batch_bytes:
                remaining = deadline - time.perf_counter()
                if remaining <= 0 or not select.select([conn], [], [], remaining)[0]:
                    break
                chunk = receive(conn, batch_bytes - size, stop_event)
                if not chunk:
                    break
                parts.append(chunk)
                size += len(chunk)
            data = b''.join(parts)
        pending.extend(data)
        messages = []
        group_command = None
        group = bytearray()

        def flush_group():
            nonlocal group_command, group
            if group_command is not None:
                messages.append((group_command, bytes(group)))
                group_command, group = None, bytearray()

        def flush():
            flush_group()
            if messages:
                nd.send_many(messages)
                messages.clear()

        while pending:
            op = pending[0]
            if op == ord('Y'):
                if len(pending) < 12:
                    break
                tick, page, register, value = struct.unpack_from('<QBBB', pending, 1)
                del pending[:12]
                if page not in (0, 1):
                    raise ValueError('invalid YM2612 page')
                if page == 0 and register == 0x2a:
                    wait = 0 if previous_dac_tick is None else tick - previous_dac_tick
                    if not 0 <= wait <= 0xffffffff:
                        raise ValueError('DAC timestamp moved backwards or wait exceeds u32')
                    previous_dac_tick = tick
                    wait, remainder = divmod(wait * scale.numerator + remainder, scale.denominator)
                    if wait > 0xffffffff:
                        raise ValueError('scaled DAC wait exceeds u32')
                    dac_count += 1
                    target = 0x1f
                    header = b'\x02\x52\0'
                    item = struct.pack('<IB', wait, value)
                    limit = nd.live_max_records
                else:
                    target = 0x52 + page
                    header = bytes((0, page))
                    item = bytes((register, value))
                    limit = nd.max_pairs
                if group_command != target or (len(group) - len(header)) // len(item) >= limit:
                    flush_group()
                    group_command, group = target, bytearray(header)
                group.extend(item)
                count += 1
            elif op == ord('G'):
                if len(pending) < 10:
                    break
                del pending[:10]  # PSG requires the ordered profile revision 5.
            elif op == ord('T'):
                if len(pending) < 9:
                    break
                del pending[:9]  # Ignore timestamps from older plugin versions.
            elif op == ord('F'):
                if len(pending) < 6:
                    break
                state, skipped = struct.unpack_from('<BI', pending, 1)
                del pending[:6]
                if state not in (0, 1):
                    raise ValueError('invalid fast-forward state')
                flush()
                previous_dac_tick = None
                print('ND6 fast-forward entered' if state else
                      f'ND6 fast-forward ended; {skipped} DAC writes skipped by MAME', flush=True)
            elif op in (ord('P'), ord('U')):
                if len(pending) < 9:
                    break
                del pending[:9]
                flush()
                if nd.volume_supported:
                    nd.request(3, b'\x60' if op == ord('P') else b'\0')
                print('ND6 muted' if op == ord('P') else 'ND6 unmuted', flush=True)
            elif op == ord('R'):
                del pending[0]
                flush()
                nd.request(0)
                previous_dac_tick = None
                remainder = 0
                print('ND6 reset', flush=True)
            elif op == ord('C'):
                if len(pending) < 5:
                    break
                clock_hz = struct.unpack_from('<I', pending, 1)[0]
                del pending[:5]
                if clock_hz not in (7670454, 7600489):
                    raise ValueError('unsupported Mega Drive YM2612 clock')
                flush()
                chip = nd.chip_clock
                if chip[4] == 2 and chip[3] & 4:
                    nd.request(4, struct.pack('<BBI', 0x52, 0, clock_hz))
                elif chip[4] != 1 or int.from_bytes(chip[5:9], 'little') != clock_hz:
                    raise RuntimeError(f'chip clock is incompatible with {clock_hz} Hz')
                print(f'ND6 YM2612 clock: {clock_hz} Hz', flush=True)
            elif op == ord('Q'):
                del pending[0]
                flush()
                return count, dac_count
            else:
                raise ValueError(f'unknown or incompatible MAME plugin command {op:02x}')
        flush()


def ordered_record(wait, page, reg, value):
    if page == 0 and reg == 0x2a:
        if wait <= 0xfd:
            return bytes((wait, value))
        return b'\xfe' + struct.pack('<IB', wait, value)
    return b'\xff' + struct.pack('<IBBB', wait, page, reg, value)


class MameClockSkip:
    """Skip stale DAC events using the source/host phase estimate.

    This cannot observe or remove events already queued on the ND6.
    """
    RATE = 44100
    TRIGGER = 441  # 10 ms: tolerate ordinary batching jitter.
    TARGET = 88  # Return the estimate to about 2 ms, rather than slow correction.
    QUIET_TICKS = RATE // 4

    def __init__(self):
        self.total_removed = 0
        self.total_dropped = 0
        self.skip_intervals = 0
        self.reset()

    def reset(self):
        self.anchor_tick = self.anchor_wall = self.last_tick = self.last_wall = None
        self.removed = self.debt = 0

    def adjust(self, tick, delta, now, is_dac=False):
        if self.anchor_tick is None:
            self.anchor_tick, self.anchor_wall = tick, now
            self.last_tick, self.last_wall = tick, now
            return delta, False

        elapsed_wall = now - self.last_wall
        elapsed_source = tick - self.last_tick
        # A real quiet interval allows the live ND6 queue to drain. Re-anchor
        # rather than turning a pause or a long rest into persistent drift.
        if (elapsed_source >= self.QUIET_TICKS and elapsed_wall >= 0.25
                and abs(elapsed_source / self.RATE - elapsed_wall) < 0.05):
            self.reset()
            return self.adjust(tick, delta, now, is_dac)
        self.last_tick, self.last_wall = tick, now
        ahead = tick - self.anchor_tick - int((now - self.anchor_wall) * self.RATE) - self.removed
        if not self.debt and ahead > self.TRIGGER:
            self.debt = ahead - self.TARGET
            self.skip_intervals += 1
        # Wall time may have advanced while input or USB was delayed. Do not
        # keep skipping an interval that has already caught up on its own.
        self.debt = min(self.debt, max(0, ahead - self.TARGET))
        skipping = self.debt > 0
        allowance = min(delta, self.debt)
        self.debt -= allowance
        self.removed += allowance
        self.total_removed += allowance
        drop = is_dac and skipping and allowance == delta
        self.total_dropped += int(drop)
        return delta - allowance, drop


def ordered_connection(nd, conn, batch_ms=2.0, batch_events=50, prefill_ms=0.0, *, stop_event=None):
    """Send one-way YM bursts on one ordered device timeline."""
    if not 1 <= batch_events <= 256:
        raise ValueError('batch event limit must be between 1 and 256')
    if not 0 <= prefill_ms <= 25:
        raise ValueError('ordered prefill must be between 0 and 25 ms')
    pending = bytearray()
    records = bytearray()
    sequence = 0
    nd.ordered_frames_sent = 0
    nd.ordered_events_sent = 0
    previous_tick = None
    previous_dac_tick = None
    catchup = MameClockSkip()
    fast_mode = False
    fast_dac_dropped = 0
    count = dac_count = 0
    prefill_pending = prefill_ms > 0
    prefill_deadline = None

    def ordered_tick(tick):
        # Different emulated CPUs can execute in separate scheduler slices.
        # A later callback can therefore carry an earlier local CPU time.
        # Preserve callback order and clamp small regressions to the most
        # recent timestamp; never move the reference backwards, which would
        # add the same time again to a subsequent wait.
        if previous_tick is not None and tick < previous_tick:
            if previous_tick - tick > 441:  # 10 ms: likely a discontinuity
                raise ValueError(f'ordered timestamp regressed by {previous_tick - tick} ticks')
            return previous_tick
        return tick

    def flush():
        nonlocal sequence
        while records:
            messages, used, next_sequence = [], 0, sequence
            total = len(records) // 7
            while used < total and len(messages) < 7:
                payload = bytearray(b'\x03' + struct.pack('<I', next_sequence))
                n = 0
                while used < total and n < nd.ordered_max_records:
                    encoded = ordered_record(*struct.unpack_from('<IBBB', records, used * 7))
                    if len(payload) + len(encoded) > nd.max_payload:
                        break
                    payload.extend(encoded)
                    used += 1
                    n += 1
                if not n:
                    raise ValueError('NDSIF payload limit is too small for an ordered event')
                messages.append((0x22, bytes(payload)))
                next_sequence = (next_sequence + 1) & 0xffffffff
            nd.send_oneway(messages)
            sequence = next_sequence
            nd.ordered_frames_sent = sequence
            nd.ordered_events_sent += used
            del records[:used*7]

    while True:
        if prefill_pending and records:
            remaining = prefill_deadline - time.perf_counter()
            if remaining <= 0 or not select.select([conn], [], [], remaining)[0]:
                flush()
                prefill_pending, prefill_deadline = False, None
                continue
        maximum = 12 * batch_events
        read_started = time.perf_counter()
        data = receive(conn, maximum, stop_event)
        # A long empty input wait lets the live queue drain, including when
        # MAME itself is stalled and its emulated time barely advances. Do not
        # carry that wall-clock gap into later drift estimates.
        if time.perf_counter() - read_started >= 0.25:
            catchup.reset()
        if not data:
            flush()
            if pending:
                raise ValueError('incomplete ordered MAME message at disconnect')
            return count, dac_count
        if batch_ms > 0:
            parts, size = [data], len(data)
            deadline = time.perf_counter() + batch_ms / 1000
            if prefill_pending and prefill_deadline is not None:
                deadline = min(deadline, prefill_deadline)
            while size < maximum:
                remaining = deadline - time.perf_counter()
                if remaining <= 0 or not select.select([conn], [], [], remaining)[0]:
                    break
                chunk = receive(conn, maximum - size, stop_event)
                if not chunk:
                    break
                parts.append(chunk); size += len(chunk)
            data = b''.join(parts)
        pending.extend(data)
        batch_time = time.perf_counter()
        while pending:
            op = pending[0]
            if op == ord('Y'):
                if len(pending) < 12:
                    break
                tick, page, reg, value = struct.unpack_from('<QBBB', pending, 1)
                del pending[:12]
                if page not in (0, 1):
                    raise ValueError('invalid YM2612 page')
                tick = ordered_tick(tick)
                delta = 0 if previous_tick is None else tick - previous_tick
                if not 0 <= delta <= 0xffffffff:
                    raise ValueError('ordered YM timestamp moved backwards or wait exceeds u32')
                previous_tick = tick
                is_dac = page == 0 and reg == 0x2a
                if fast_mode:
                    delta, drop = 0, is_dac
                    fast_dac_dropped += int(drop)
                else:
                    delta, drop = catchup.adjust(tick, delta, batch_time, is_dac)
                count += 1
                dac_count += int(is_dac)
                # Rebuild a small lead at a new PCM phrase. A startup-only
                # prefill often drains during the quiet logo/initialization.
                if (is_dac and prefill_ms and not prefill_pending
                        and (previous_dac_tick is None or tick - previous_dac_tick >= 882)):
                    flush()
                    prefill_pending, prefill_deadline = True, None
                if is_dac:
                    previous_dac_tick = tick
                if drop:
                    continue
                if prefill_pending and prefill_deadline is None:
                    prefill_deadline = time.perf_counter() + prefill_ms / 1000
                records.extend(struct.pack('<IBBB', delta, page, reg, value))
            elif op == ord('G'):
                if len(pending) < 10:
                    break
                tick, value = struct.unpack_from('<QB', pending, 1)
                del pending[:10]
                if not nd.ordered_psg:
                    continue
                tick = ordered_tick(tick)
                delta = 0 if previous_tick is None else tick - previous_tick
                if not 0 <= delta <= 0xffffffff:
                    raise ValueError('ordered PSG timestamp moved backwards or wait exceeds u32')
                previous_tick = tick
                if fast_mode:
                    delta = 0
                else:
                    delta, _ = catchup.adjust(tick, delta, batch_time)
                if prefill_pending and prefill_deadline is None:
                    prefill_deadline = time.perf_counter() + prefill_ms / 1000
                records.extend(struct.pack('<IBBB', delta, 2, 0, value))
                count += 1
            elif op == ord('F'):
                if len(pending) < 6:
                    break
                state, plugin_skipped = struct.unpack_from('<BI', pending, 1)
                del pending[:6]
                if state not in (0, 1):
                    raise ValueError('invalid fast-forward state')
                flush()
                fast_mode = bool(state)
                catchup.reset()
                previous_tick = previous_dac_tick = None
                prefill_pending, prefill_deadline = False, None
                if fast_mode:
                    fast_dac_dropped = 0
                    print('ND6 fast-forward: DAC suppressed, FM/PSG kept', flush=True)
                else:
                    print(f'ND6 fast-forward ended: {plugin_skipped + fast_dac_dropped} DAC writes skipped', flush=True)
            elif op in (ord('P'), ord('U')):
                if len(pending) < 9:
                    break
                del pending[:9]
                had_records = bool(records)
                flush()
                if had_records:
                    prefill_pending, prefill_deadline = False, None
                if nd.volume_supported:
                    nd.request(3, b'\x60' if op == ord('P') else b'\0')
                catchup.reset()
            elif op == ord('R'):
                del pending[0]
                flush()
                nd.request(0)
                previous_tick, sequence = None, 0
                nd.ordered_frames_sent = 0
                nd.ordered_events_sent = 0
                previous_dac_tick = None
                catchup.reset()
                fast_mode = False
                fast_dac_dropped = 0
                prefill_pending, prefill_deadline = prefill_ms > 0, None
                print('ND6 reset', flush=True)
            elif op == ord('C'):
                if len(pending) < 5:
                    break
                clock_hz = struct.unpack_from('<I', pending, 1)[0]
                del pending[:5]
                if clock_hz not in (7670454, 7600489):
                    raise ValueError('unsupported Mega Drive YM2612 clock')
                had_records = bool(records)
                flush()
                if had_records:
                    prefill_pending, prefill_deadline = False, None
                if nd.ordered_psg:
                    nd.set_psg_clock(3546895 if clock_hz == 7600489 else 3579545)
                chip = nd.chip_clock
                if chip[4] == 2 and chip[3] & 4:
                    nd.request(4, struct.pack('<BBI', 0x52, 0, clock_hz))
                elif chip[4] != 1 or int.from_bytes(chip[5:9], 'little') != clock_hz:
                    raise RuntimeError(f'chip clock is incompatible with {clock_hz} Hz')
            elif op == ord('Q'):
                del pending[0]
                flush()
                if catchup.total_removed:
                    print(f'ND6 clock skip: {catchup.total_removed / 44.1:.2f} ms, '
                          f'{catchup.total_dropped} DAC writes skipped, '
                          f'{catchup.skip_intervals} intervals', flush=True)
                return count, dac_count
            else:
                raise ValueError(f'unknown or incompatible ordered MAME plugin command {op:02x}')
        if (not prefill_pending or len(records) // 7 >= min(256, nd.ordered_capacity // 2 or 1)
                or (prefill_deadline is not None and time.perf_counter() >= prefill_deadline)):
            flush()
            if prefill_pending:
                prefill_pending, prefill_deadline = False, None


def read_nd6_diagnostics(nd, ordered=False):
    # Temporary ND6-specific command, not part of the NDSIF v1 draft.
    if ordered:
        result = nd.request(0x20, b'N6D3')
        if len(result) != 54 or result[1] != 3:
            raise ValueError('invalid ND6 ordered diagnostic response')
        keys = ('queued', 'peak', 'accepted', 'dequeued', 'full_retries',
                'late_over_100us', 'late_after_empty', 'worst_late_us',
                'first_us', 'last_us', 'source_ticks')
        return dict(zip(keys, struct.unpack_from('<HH6I3Q', result, 2)))
    result = nd.request(0x20, b'N6D1')
    if len(result) != 30 or result[1] != 1:
        raise ValueError('invalid ND6 diagnostic response')
    keys = ('queued', 'peak', 'accepted', 'dequeued', 'dropped_frames',
            'dropped_records', 'late_after_empty', 'worst_late_us')
    return dict(zip(keys, struct.unpack_from('<HH6I', result, 2)))


def read_ordered_status(nd):
    result = nd.request(0x23)
    if len(result) != 18 or result[1] != 3:
        raise ValueError('invalid ordered live status')
    keys = ('next_sequence', 'accepted', 'queued', 'free', 'missing_frames')
    status = dict(zip(keys, struct.unpack_from('<IIHHI', result, 2)))
    if status['queued'] + status['free'] != nd.ordered_capacity:
        raise ValueError('invalid ordered live queue status')
    return status


def serve_sessions(nd, handler, *, stop_event=None, once=False, after_session=None):
    """Accept one active MAME and reject concurrent clients immediately."""
    stop_event = stop_event or threading.Event()
    active = None
    worker = None
    fatal = []
    with socket.socket() as listener:
        if hasattr(socket, 'SO_EXCLUSIVEADDRUSE'):
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        listener.bind(('127.0.0.1', LISTEN_PORT))
        listener.listen(8)
        listener.settimeout(0.2)
        print(f'Waiting for MAME plugin on 127.0.0.1:{LISTEN_PORT}...', flush=True)

        def session(conn):
            try:
                with conn:
                    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                    conn.settimeout(0.2)
                    handler(conn)
            except (OSError, ValueError, RuntimeError, TimeoutError) as exc:
                if not stop_event.is_set():
                    print(f'MAME session ended: {exc}', file=sys.stderr, flush=True)
            finally:
                try:
                    nd.request(0)
                    if nd.volume_supported:
                        nd.request(3, b'\0')
                    if after_session:
                        after_session()
                except Exception as exc:
                    fatal.append(exc)

        try:
            while not stop_event.is_set():
                if worker and not worker.is_alive():
                    worker.join()
                    worker, active = None, None
                    if fatal:
                        raise RuntimeError(f'NDSIF session cleanup failed: {fatal[0]}') from fatal[0]
                    if once:
                        break
                    print(f'Waiting for MAME plugin on 127.0.0.1:{LISTEN_PORT}...', flush=True)
                try:
                    conn, _ = listener.accept()
                except socket.timeout:
                    continue
                if worker is not None:
                    print('MAME connection rejected: another instance is active', flush=True)
                    conn.close()
                    continue
                active = conn
                print('MAME connected', flush=True)
                worker = threading.Thread(target=session, args=(conn,), name='mame-session')
                worker.start()
        finally:
            stop_event.set()
            if active is not None:
                try:
                    active.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            if worker:
                worker.join()
            if fatal:
                raise RuntimeError(f'NDSIF session cleanup failed: {fatal[0]}') from fatal[0]


def run_live(port, batch_ms=5.0, dac_wait_scale=Fraction(1), batch_events=100,
             nd6_diagnostics=False, ordered=True, prefill_ms=0.0, *, stop_event=None, once=False):
    stop_event = stop_event or threading.Event()
    if ordered and Fraction(str(dac_wait_scale)) != 1:
        raise ValueError('DAC-only wait scaling is incompatible with an ordered YM timeline')
    nd = NDSIF(port, 'nd6')
    initialized = False
    try:
        nd.setup(live=True, ordered=ordered)
        initialized = True
        print(f'ND6 input batch: {batch_ms:g} ms / {batch_events} events', flush=True)

        def handle(conn):
            if ordered:
                print('ND6 ordered YM timeline enabled', flush=True)
                count, dac_count = ordered_connection(nd, conn, batch_ms, batch_events, prefill_ms, stop_event=stop_event)
            else:
                count, dac_count = live_connection(nd, conn, batch_ms, dac_wait_scale, batch_events, stop_event=stop_event)
            print(f'MAME stopped; {count} input register writes ({dac_count} DAC writes)', flush=True)
            if nd6_diagnostics:
                if ordered:
                    status = read_ordered_status(nd)
                    print('ND6 ordered final status: ' + ', '.join(f'{key}={value}' for key, value in status.items()), flush=True)
                    if (status['next_sequence'] != nd.ordered_frames_sent
                            or status['accepted'] != (nd.ordered_events_sent & 0xffffffff)
                            or status['missing_frames']):
                        print(f"ND6 ordered transfer mismatch: sent_frames={nd.ordered_frames_sent}, sent_events={nd.ordered_events_sent}", file=sys.stderr, flush=True)
                stats = read_nd6_diagnostics(nd, ordered=ordered)
                print('ND6 diagnostics: ' + ', '.join(f'{key}={value}' for key, value in stats.items()), flush=True)
                if ordered and stats['dequeued'] > 1:
                    actual = (stats['last_us'] - stats['first_us']) / 1_000_000
                    source = stats['source_ticks'] / 44100
                    print(f'ND6 scheduler span: source={source:.3f}s actual={actual:.3f}s', flush=True)

        def restore_clocks():
            chip = nd.chip_clock
            if chip[4] == 2 and chip[3] & 4:
                nd.request(4, struct.pack('<BBI', 0x52, 0, 7670454))
            if ordered and nd.ordered_psg:
                nd.set_psg_clock(3579545)

        serve_sessions(nd, handle, stop_event=stop_event, once=once, after_session=restore_clocks)
    finally:
        if initialized:
            try:
                nd.request(0)
            except Exception as exc:
                print(f'NDSIF cleanup reset failed: {exc}', file=sys.stderr)
        nd.serial.close()


def mame_executable(pid):
    """Locate the running MAME executable without depending on the working directory."""
    if sys.platform == 'win32':
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.QueryFullProcessImageNameW.argtypes = (wintypes.HANDLE, wintypes.DWORD,
                                                      wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD))
        kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
        kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
        kernel.CloseHandle.restype = wintypes.BOOL
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            raise OSError(ctypes.get_last_error(), 'cannot inspect the MAME process')
        try:
            size = wintypes.DWORD(32768)
            path = ctypes.create_unicode_buffer(size.value)
            if not kernel.QueryFullProcessImageNameW(handle, 0, path, ctypes.byref(size)):
                raise OSError(ctypes.get_last_error(), 'cannot get the MAME executable path')
            result = Path(path.value)
        finally:
            kernel.CloseHandle(handle)
    elif sys.platform.startswith('linux'):
        result = Path(os.readlink(f'/proc/{pid}/exe'))
    else:
        raise RuntimeError('automatic MAME clock discovery is not supported on this OS')
    if not re.fullmatch(r'mame[\w.-]*', result.stem, re.IGNORECASE):
        raise ValueError(f'cannot identify MAME executable: {result.name}')
    return result


def mame_ym2151_clock(pid, machine, tag):
    if not re.fullmatch(r'[a-z0-9_]+', machine) or not tag.startswith(':'):
        raise ValueError('invalid MAME machine name or YM2151 tag')
    executable = mame_executable(pid)
    try:
        result = subprocess.run([str(executable), '-noreadconfig', '-listxml', machine],
                                cwd=str(executable.parent), capture_output=True, timeout=20,
                                creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0), check=True)
        root = ET.fromstring(result.stdout)
    except (OSError, subprocess.SubprocessError, ET.ParseError) as exc:
        raise RuntimeError(f'cannot query MAME clock for {machine}: {exc}') from exc
    system = next((item for item in root.findall('machine') if item.get('name') == machine), None)
    if system is None:
        raise ValueError(f'MAME XML has no machine {machine}')
    refs = [item for item in system.findall('device_ref')
            if item.get('tag', '').lstrip(':') == tag.lstrip(':') and item.get('name') == 'ym2151']
    chips = [item for item in system.findall('chip')
             if item.get('type') == 'audio' and item.get('tag', '').lstrip(':') == tag.lstrip(':')]
    if len(refs) != 1 or len(chips) != 1 or not chips[0].get('clock', '').isdigit():
        raise ValueError(f'MAME XML has no unambiguous YM2151 clock for {tag}')
    hz = int(chips[0].get('clock'))
    if not 0 < hz <= 0xffffffff:
        raise ValueError('invalid YM2151 clock')
    return hz


def nd8_connection(nd, conn, *, stop_event=None):
    pending = bytearray()
    count = 0
    clock_cache = {}
    while True:
        data = receive(conn, 8192, stop_event)
        if not data:
            return
        pending.extend(data)
        pairs = bytearray()
        while pending:
            op = pending[0]
            if op == ord('I'):
                if len(pending) < 7:
                    break
                pid, game_len, tag_len = struct.unpack_from('<IBB', pending, 1)
                size = 7 + game_len + tag_len
                if len(pending) < size:
                    break
                if pairs:
                    nd.writes(pairs)
                    pairs.clear()
                game = bytes(pending[7:7 + game_len]).decode('ascii')
                tag = bytes(pending[7 + game_len:size]).decode('ascii')
                del pending[:size]
                key = (pid, game, tag)
                if key not in clock_cache:
                    clock_cache[key] = mame_ym2151_clock(*key)
                hz = clock_cache[key]
                chip = nd.chip_clock
                if chip[4] == 2 and chip[3] & 4:
                    nd.request(4, struct.pack('<BBI', 0x54, 0, hz))
                    # Current ND8 firmware normalizes requests to its clock
                    # table; the protocol response does not report the result.
                    if hz not in (3375000, 3500000, 4000000, 4026987) and not 3579000 <= hz <= 3580000:
                        print(f'ND8: {hz} Hz is not an exact supported clock; firmware may use 3579545 Hz',
                              file=sys.stderr, flush=True)
                elif chip[4] != 1 or int.from_bytes(chip[5:9], 'little') != hz:
                    raise RuntimeError(f'YM2151 clock is incompatible with {hz} Hz')
                print(f'ND8 YM2151 clock requested: {hz} Hz ({game}, {tag})', flush=True)
                continue
            if op == ord('W'):
                if len(pending) < 3:
                    break
                pairs.extend(pending[1:3])
                del pending[:3]
                count += 1
                continue
            if pairs:
                nd.writes(pairs)
                pairs.clear()
            del pending[0]
            if op == ord('R'):
                nd.request(0)
                print('ND8 reset', flush=True)
            elif op == ord('P'):
                if nd.volume_supported:
                    nd.request(3, b'\x60')
                    print('Paused: ND8 muted', flush=True)
                else:
                    print('Paused: ND8 has no output mute', flush=True)
            elif op == ord('U'):
                if nd.volume_supported:
                    nd.request(3, b'\0')
                    print('Resumed: ND8 unmuted', flush=True)
            elif op == ord('Q'):
                print(f'MAME stopped; {count} register writes', flush=True)
                return
            else:
                raise ValueError(f'unknown or incompatible MAME plugin command {op:02x}')
        if pairs:
            nd.writes(pairs)


def run(port, device='nd8', *, stop_event=None, once=False):
    stop_event = stop_event or threading.Event()
    if device != 'nd8':
        raise ValueError('use run_live for the ND6 live DAC profile')
    nd = NDSIF(port, device)
    initialized = False
    try:
        nd.setup()
        initialized = True
        serve_sessions(nd, lambda conn: nd8_connection(nd, conn, stop_event=stop_event), stop_event=stop_event, once=once)
    finally:
        if initialized:
            try:
                nd.request(0)
            except Exception as exc:
                print(f'NDSIF cleanup reset failed: {exc}', file=sys.stderr)
        nd.serial.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default=None, help='serial port (ND6: COM5, ND8: COM3)')
    parser.add_argument('--device', choices=('nd8', 'nd6'), default='nd6')
    parser.add_argument('--batch-ms', type=float, default=5.0,
                        help='ND6 input batching deadline in milliseconds (0 to 25; default 5)')
    parser.add_argument('--batch-events', type=int, default=100,
                        help='ND6 input batch size in event equivalents (1 to 256; default 100)')
    parser.add_argument('--dac-wait-scale', type=Fraction, default=Fraction(1),
                        help='ND6 DAC wait multiplier (greater than 0 and at most 4; default 1)')
    parser.add_argument('--nd6-diagnostics', action='store_true',
                        help='query experimental ND6 queue statistics after MAME stops (ordered requires 3.1dev21)')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--ordered', dest='ordered', action='store_true', default=True,
                      help='use ordered FM/DAC/PSG timeline (default)')
    mode.add_argument('--live-dac', dest='ordered', action='store_false',
                      help='use legacy live DAC profile 2')
    parser.add_argument('--once', action='store_true', help='exit after one MAME session')
    parser.add_argument('--prefill-ms', type=float, default=0.0,
                        help='experimental ordered startup and PCM-phrase prefill (0 to 25 ms; default 0)')
    args = parser.parse_args()
    if args.port is None:
        args.port = 'COM5' if args.device == 'nd6' else 'COM3'
    if not 0 <= args.batch_ms <= 25:
        parser.error('--batch-ms must be between 0 and 25')
    if not 1 <= args.batch_events <= 256:
        parser.error('--batch-events must be between 1 and 256')
    if not 0 < args.dac_wait_scale <= 4:
        parser.error('--dac-wait-scale must be greater than 0 and at most 4')
    if not 0 <= args.prefill_ms <= 25 or (args.prefill_ms and not args.ordered):
        parser.error('--prefill-ms must be 0 to 25 and requires --ordered when nonzero')
    try:
        if args.device == 'nd6':
            run_live(args.port, args.batch_ms, args.dac_wait_scale, args.batch_events, args.nd6_diagnostics, args.ordered, args.prefill_ms, once=args.once)
        else:
            run(args.port, args.device, once=args.once)
    except (OSError, ValueError, RuntimeError, TimeoutError, serial.SerialException) as exc:
        print(f'NDSIF bridge: {exc}', file=sys.stderr)
        sys.exit(1)
