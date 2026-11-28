"""Reject packages that cannot pass the Adafruit INIT device-type check."""
import struct
import sys
import zipfile

with zipfile.ZipFile(sys.argv[1]) as z:
    init = z.read(next(name for name in z.namelist() if name.endswith(".dat")))
    device, revision = struct.unpack_from("<HH", init)
    if (device, revision) != (0x0052, 52840):
        raise SystemExit(f"Invalid DFU device metadata: {device:#06x}, {revision}")
    print(f"DFU metadata verified: device={device:#06x}, revision={revision}")
