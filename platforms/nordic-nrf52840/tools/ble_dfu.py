#!/usr/bin/env python3
"""Application-only Nordic Legacy DFU for the ANCHOR bench bootloader."""

import argparse
import asyncio
import re
import struct
import sys
import time

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

from usb_dfu import package_info


SERVICE = "00001530-1212-efde-1523-785feabcd123"
CONTROL = "00001531-1212-efde-1523-785feabcd123"
PACKET = "00001532-1212-efde-1523-785feabcd123"
INIT_PACKET_SIZE = 20
MAX_FIRMWARE_PACKET_SIZE = 244
RECEIPT_INTERVAL = 8


async def firmware_packet_size(client, packet_char):
    write_limit = packet_char.max_write_without_response_size
    if write_limit == INIT_PACKET_SIZE and sys.platform.startswith("linux"):
        # BlueZ exposes the live MTU through AcquireWrite. Bleak's MTU example
        # uses this backend method to query it and release the write socket.
        acquire_mtu = getattr(client._backend, "_acquire_mtu", None)
        if acquire_mtu is not None:
            try:
                await asyncio.wait_for(acquire_mtu(), timeout=3)
            except (BleakError, TimeoutError):
                pass
            else:
                write_limit = client.mtu_size - 3
    # S140's Legacy DFU flash writes are word-aligned.
    return min(write_limit, MAX_FIRMWARE_PACKET_SIZE) & ~3


async def transfer(device, init_packet, firmware):
    disconnected = asyncio.Event()
    async with BleakClient(device, disconnected_callback=lambda _: disconnected.set()) as client:
        service = client.services.get_service(SERVICE)
        if service is None:
            raise RuntimeError("Target does not expose the Nordic Legacy DFU service")
        control_char = service.get_characteristic(CONTROL)
        packet_char = service.get_characteristic(PACKET)
        if control_char is None or packet_char is None:
            raise RuntimeError("Target is missing the Nordic Legacy DFU characteristics")

        packet_size = await firmware_packet_size(client, packet_char)
        print(f"Firmware writes use {packet_size} bytes per packet", flush=True)

        notifications = asyncio.Queue()
        await client.start_notify(control_char, lambda _, value: notifications.put_nowait(bytes(value)))

        async def control(*data):
            await client.write_gatt_char(control_char, bytes(data), response=True)

        async def packet(data):
            await client.write_gatt_char(packet_char, data, response=False)

        async def receive(expected, timeout=8):
            answer = await asyncio.wait_for(notifications.get(), timeout)
            if answer != expected:
                raise RuntimeError(f"Expected {expected.hex(' ')}, received {answer.hex(' ')}")

        started = time.monotonic()
        await control(1, 4)  # START, application only
        await packet(struct.pack("<III", 0, 0, len(firmware)))
        await receive(b"\x10\x01\x01", timeout=30)
        print("START accepted", flush=True)

        await control(2, 0)
        for offset in range(0, len(init_packet), INIT_PACKET_SIZE):
            await packet(init_packet[offset:offset + INIT_PACKET_SIZE])
        await control(2, 1)
        await receive(b"\x10\x02\x01")
        print("INIT accepted", flush=True)

        await control(8, RECEIPT_INTERVAL, 0)
        await control(3)
        packet_count = 0
        next_report = 65536
        for offset in range(0, len(firmware), packet_size):
            chunk = firmware[offset:offset + packet_size]
            await packet(chunk)
            packet_count += 1
            sent = offset + len(chunk)
            if packet_count % RECEIPT_INTERVAL == 0 and sent < len(firmware):
                await receive(b"\x11" + struct.pack("<I", sent))
            if sent >= next_report:
                print(f"Sent {sent}/{len(firmware)} bytes", flush=True)
                next_report += 65536
        await receive(b"\x10\x03\x01", timeout=20)
        await control(4)
        await receive(b"\x10\x04\x01", timeout=20)
        print(f"Bootloader validated {len(firmware)} bytes in {time.monotonic() - started:.3f} s", flush=True)

        try:
            await control(5)
        except BleakError:
            if client.is_connected:
                raise
        await asyncio.wait_for(disconnected.wait(), 10)
        print("Target disconnected after ACTIVATE. Verify the application restarted.", flush=True)


async def update(address, init_packet, firmware):
    def matches(device, advertisement):
        return (device.address.upper() == address and
                advertisement.local_name == "AdaDFU" and
                SERVICE in advertisement.service_uuids)

    device = await BleakScanner.find_device_by_filter(matches, timeout=20)
    if not device:
        raise RuntimeError(f"AdaDFU at {address} was not found")
    await transfer(device, init_packet, firmware)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", required=True, help="AdaDFU address shown by the TX15")
    parser.add_argument("--package", required=True, help="ANCHOR application-only DFU ZIP")
    args = parser.parse_args()
    address = args.address.upper()
    if not re.fullmatch(r"(?:[0-9A-F]{2}:){5}[0-9A-F]{2}", address):
        parser.error("--address must be the colon-separated Bluetooth address")
    init_packet, firmware, digest, _ = package_info(args.package)
    print(f"Target {address}, image {len(firmware)} bytes, SHA-256 {digest}", flush=True)
    asyncio.run(update(address, init_packet, firmware))


if __name__ == "__main__":
    main()
