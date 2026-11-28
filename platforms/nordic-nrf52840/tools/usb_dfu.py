#!/usr/bin/env python3
"""Nordic nRF52840 application DFU through the EdgeTX Bluetooth USB bridge.

Uses Nordic's adafruit-nrfutil DfuTransportSerial/HCI/SLIP implementation.
The EdgeTX CDC bridge remains the USB transport; this script only adds the
bridge management handshake around the stock Nordic serial DFU transport.
"""
import argparse
import hashlib
import json
import logging
import sys
import time
import zipfile

from nordicsemi.dfu import crc16
from nordicsemi.dfu.dfu_transport_serial import DfuTransportSerial, HciPacket

SUPPORTED_BAUDS = (115200, 460800)
UPDATE_MODE_APP = 4
BRIDGE_FINISH = "OK+BRIDGE:FINISH"


def package_info(path):
    with zipfile.ZipFile(path) as zf:
        manifest = json.loads(zf.read("manifest.json"))["manifest"]
        if set(manifest) - {"dfu_version"} != {"application"}:
            raise ValueError("Package must contain application firmware only")
        app = manifest["application"]
        init_packet = zf.read(app["dat_file"])
        firmware = zf.read(app["bin_file"])
        meta = app.get("init_packet_data", {})
        if meta.get("device_revision") != 52840:
            raise ValueError("DFU package is not identified for nRF52840")
        if meta.get("device_type") != 82:
            raise ValueError("Unexpected ANCHOR device type in DFU package")
        if not firmware or not init_packet:
            raise ValueError("Application firmware or init packet is empty")
        actual_crc = crc16.calc_crc16(firmware, crc=0xffff)
        if actual_crc != meta.get("firmware_crc16"):
            raise ValueError("Application CRC16 does not match Nordic init packet")
        return init_packet, firmware, hashlib.sha256(firmware).hexdigest(), meta


def write_line(port, line):
    port.write(line.encode("ascii") + b"\r\n")
    port.flush()


def read_line(port, timeout, description):
    deadline = time.monotonic() + timeout
    data = bytearray()
    while time.monotonic() < deadline:
        chunk = port.read(1)
        if not chunk:
            continue
        if chunk in (b"\r", b"\n"):
            if data:
                line = data.decode("ascii", errors="replace")
                print("<", line, flush=True)
                return line
        else:
            data.extend(chunk)
            if len(data) > 512:
                data.clear()
    raise TimeoutError("Timed out waiting for " + description)


def wait_for_prefix(port, prefix, timeout, description):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = read_line(port, max(0.05, deadline-time.monotonic()), description)
        if line.startswith(prefix):
            return line
    raise TimeoutError("Timed out waiting for " + description)


def open_transport(port_name, baud):
    # Skip the baud-touch open. Stock nrfutil toggles DTR during this open.
    # Keep this CDC session from the bridge probe through transfer completion.
    transport = DfuTransportSerial(
        port_name, baud_rate=baud, single_bank=True, touch=0, timeout=0.25
    )
    transport.open()
    return transport


def bridge_probe(transport, baud):
    port = transport.serial_port
    status = None
    for attempt in range(12):
        write_line(port, "AT+BRIDGE=STATUS?")
        try:
            status = wait_for_prefix(port, "OK+BRIDGE:STATUS,", 0.6, "bridge status")
            break
        except TimeoutError:
            if attempt == 11:
                raise
            time.sleep(0.1)
    print("Bridge:", status, flush=True)
    write_line(port, "AT+BRIDGE?")
    line = wait_for_prefix(port, "OK+BRIDGE:", 3.0, "bridge protocol banner")
    ready = f"OK+BRIDGE:2,{baud},READY"
    recovery = f"OK+BRIDGE:2,{baud},RECOVERY"
    if line not in (ready, recovery):
        raise RuntimeError(f"Expected protocol 2 at {baud} baud, received {line!r}")
    return line == recovery


def send_finish(port_name, baud):
    # FINISH is deliberately sent on a fresh CDC connection and before PROBE.
    finish = open_transport(port_name, baud)
    try:
        time.sleep(0.25)
        write_line(finish.serial_port, "AT+BRIDGE=STATUS?")
        try:
            wait_for_prefix(finish.serial_port, "OK+BRIDGE:STATUS,", 1.0,
                            "post-transfer bridge counters")
        except TimeoutError:
            print("Post-transfer bridge counters unavailable.", flush=True)
        write_line(finish.serial_port, "AT+BRIDGE=FINISH")
        line = wait_for_prefix(finish.serial_port, BRIDGE_FINISH, 4.0, "FINISH acknowledgement")
        if line != BRIDGE_FINISH:
            raise RuntimeError("Unexpected FINISH response: " + line)
        print("EdgeTX accepted FINISH; waiting for Bluetooth role restoration.", flush=True)
    finally:
        finish.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True, help="TX15 USB CDC serial port (COMx or /dev/ttyACM*)")
    ap.add_argument("--package", required=True, help="Nordic application DFU ZIP (manifest + .dat + .bin)")
    ap.add_argument("--baud", type=int, choices=SUPPORTED_BAUDS, default=115200,
                    help="Require this downstream DFU rate in the bridge banner")
    ap.add_argument("--recover", action="store_true", help="Require an already-entered bootloader recovery session")
    ap.add_argument("--inspect", action="store_true", help="Validate and describe the package without opening the port")
    args = ap.parse_args()

    init_packet, firmware, digest, meta = package_info(args.package)
    print("Package:", args.package)
    print("Application:", len(firmware), "bytes")
    print("Init packet:", len(init_packet), "bytes")
    print("Device revision:", meta.get("device_revision"))
    print("Device type:", meta.get("device_type"))
    print("Firmware CRC16:", meta.get("firmware_crc16"))
    print("SHA-256:", digest)
    if args.inspect:
        return 0

    class PacketCounter(logging.Handler):
        attempts = 0

        def emit(self, record):
            if record.msg.startswith("PC -> target: "):
                self.attempts += 1

    packet_counter = PacketCounter()
    transport_log = logging.getLogger(DfuTransportSerial.__module__)
    transport_log.setLevel(logging.DEBUG)
    transport_log.addHandler(packet_counter)
    transport_log.propagate = False
    HciPacket.sequence_number = 0
    transport = open_transport(args.port, args.baud)
    success = False
    try:
        if bridge_probe(transport, args.baud):
            print("Existing bootloader recovery session detected; resuming without AT+DOWNLOAD.", flush=True)
        else:
            if args.recover:
                raise RuntimeError("--recover was requested, but the bridge reports READY, not RECOVERY")
            write_line(transport.serial_port, "AT+DOWNLOAD")
            ack = wait_for_prefix(transport.serial_port, "OK+DOWNLOAD", 3.0,
                                  "module DOWNLOAD acknowledgement")
            if ack != "OK+DOWNLOAD":
                raise RuntimeError("Unexpected DOWNLOAD response: " + ack)
            print("Module accepted DOWNLOAD and is entering Nordic DFU.", flush=True)
            time.sleep(1.2)

        transport.serial_port.reset_input_buffer()
        started = time.monotonic()
        transport.send_start_dfu(UPDATE_MODE_APP, 0, 0, len(firmware))
        print("Nordic START accepted; application erase wait completed.", flush=True)
        transport.send_init_packet(init_packet)
        print("Nordic INIT packet accepted.", flush=True)
        transport.send_firmware(firmware)
        print("Application DATA and STOP packets accepted.", flush=True)
        transport.send_activate_firmware()
        print(f"Nordic START through STOP took {time.monotonic() - started:.3f} s.", flush=True)
        block_size = DfuTransportSerial.DFU_PACKET_MAX_SIZE
        expected_packets = (len(firmware) + block_size - 1) // block_size + 3
        print(f"HCI packet sends: {packet_counter.attempts} ({expected_packets} expected).",
              flush=True)
        success = True
    finally:
        transport.close()

    if not success:
        return 2

    time.sleep(2.0)
    send_finish(args.port, args.baud)
    print("DFU transfer completed. Check the TX15 dialog for Bluetooth restoration.", flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print("ERROR:", type(exc).__name__ + ":", exc, file=sys.stderr, flush=True)
        print("If bootloader entry already occurred, leave the radio connected and retry with --recover.", file=sys.stderr)
        sys.exit(1)
