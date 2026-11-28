#!/usr/bin/env python3
"""Application-only Nordic Legacy DFU for the ANCHOR bench bootloader."""

import argparse
import asyncio
import re
import struct
import time

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

from usb_dfu import package_info


SERVICE = "00001530-1212-efde-1523-785feabcd123"
CONTROL = "00001531-1212-efde-1523-785feabcd123"
PACKET = "00001532-1212-efde-1523-785feabcd123"
PACKET_SIZE = 20
RECEIPT_INTERVAL = 8


async def transfer(device, init_packet, firmware):
    disconnected = asyncio.Event()
    async with BleakClient(device, disconnected_callback=lambda _: disconnected.set()) as client:
        service = client.services.get_service(SERVICE)
        if not service or not service.get_characteristic(CONTROL) or not service.get_characteristic(PACKET):
            raise RuntimeError("Target does not expose the Nordic Legacy DFU service")

        notifications = asyncio.Queue()
        await client.start_notify(CONTROL, lambda _, value: notifications.put_nowait(bytes(value)))

        async def control(*data):
            await client.write_gatt_char(CONTROL, bytes(data), response=True)

        async def packet(data):
            await client.write_gatt_char(PACKET, data, response=False)

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
        for offset in range(0, len(init_packet), PACKET_SIZE):
            await packet(init_packet[offset:offset + PACKET_SIZE])
        await control(2, 1)
        await receive(b"\x10\x02\x01")
        print("INIT accepted", flush=True)

        await control(8, RECEIPT_INTERVAL, 0)
        await control(3)
        packet_count = 0
        next_report = 65536
        for offset in range(0, len(firmware), PACKET_SIZE):
            chunk = firmware[offset:offset + PACKET_SIZE]
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
