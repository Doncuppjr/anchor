// Pinned maintained Espressif flasher; ROM mode avoids UART reset wiring and
// stub-specific reset assumptions. No CLI command is required from the user.
const hex = bytes => Array.from(bytes, b => b.toString(16).padStart(2, "0")).join("");
export function normalizeRomMd5(value) {
  if (/^[0-9a-f]{64}$/i.test(value))
    value = String.fromCharCode(...value.match(/../g).map(x => parseInt(x, 16)));
  if (!/^[0-9a-f]{32}$/i.test(value)) throw new Error("Invalid flash checksum response");
  return value.toLowerCase();
}
export async function verifyFullImage(meta, data, board, profile, family, sha256) {
  if (meta.board !== board || meta.profile !== profile ||
      meta.chipFamily !== family || meta.address !== 0 ||
      !Number.isInteger(meta.bootOffset) || !Number.isInteger(meta.chipId) ||
      !/^[a-z0-9_.-]+\.bin$/i.test(meta.path))
    throw new Error("Wrong firmware manifest");
  if (data.length !== meta.size || hex(await sha256(data)) !== meta.sha256)
    throw new Error("Firmware SHA-256 integrity check failed");
  const app = 0x10000;
  if (data[meta.bootOffset] !== 0xe9 || data[0x8000] !== 0xaa ||
      data[0x8001] !== 0x50 || data[app] !== 0xe9 ||
      (data[app+12] | (data[app+13] << 8)) !== meta.chipId)
    throw new Error("Invalid full firmware image");
}
// esptool 5.3.1 target-specific RTC watchdog reset sequence. esptool-js
// 0.7.1 leaves the C5 implementation as a TODO. A full RTC/system reset
// restores pads/clocks and clears the force-download state; FLASH_END alone
// did not restore the application on the installed C5 hardware.
export async function restartRadioModule(loader, family, settle) {
  const registers = {
    "ESP32-C3":[0x60008090,0x60008094,0x600080a8],
    "ESP32-C5":[0x600b1c00,0x600b1c04,0x600b1c18]
  }[family];
  if (!registers) throw new Error("Unsupported module restart target");
  const [config, timeout, protect] = registers;
  await loader.writeReg(protect, 0x50d83aa1);
  await loader.writeReg(timeout, 2000);
  await loader.writeReg(config, 0xd0000102);
  await loader.writeReg(protect, 0);
  await settle(500);
}
async function defaultDependencies() {
  const [{ESPLoader, Transport}, {md5}] = await Promise.all([
    import("https://esm.sh/esptool-js@0.7.1"),
    import("https://esm.sh/@noble/hashes@1.8.0/legacy")
  ]);
  return {ESPLoader, Transport, md5};
}
export async function readBridgeLine(port, prefix, signal, timeoutMs = 8000, retryCommand) {
  const reader = port.readable.getReader();
  let timer, retryTimer, abort, success = false;
  try {
    const failed = new Promise((_, reject) => {
      timer = setTimeout(() => {
        reader.cancel().catch(() => {});
        reject(new Error("Module/bridge acknowledgement timed out"));
      }, timeoutMs);
      abort = () => {
        reader.cancel().catch(() => {});
        reject(new Error("Canceled"));
      };
      signal?.addEventListener("abort", abort, {once:true});
    });
    let text = "";
    const retry = Symbol("retry");
    let pendingRead = reader.read();
    for (;;) {
      if (signal?.aborted) throw new Error("Canceled");
      const waiting = [pendingRead, failed];
      if (retryCommand) waiting.push(new Promise(resolve => {
        retryTimer = setTimeout(() => resolve(retry), 500);
      }));
      const result = await Promise.race(waiting);
      clearTimeout(retryTimer);
      if (result === retry) {
        const writer = port.writable.getWriter();
        try {await writer.write(new TextEncoder().encode(retryCommand));}
        finally {writer.releaseLock();}
        continue; // Keep the same outstanding read; do not cancel the stream.
      }
      if (result.done) throw new Error("Serial connection closed");
      text += new TextDecoder().decode(result.value);
      if (text.length > 4096) throw new Error("Unexpected serial data");
      let end;
      while ((end = text.indexOf("\n")) >= 0) {
        const line = text.slice(0, end).replace(/\r$/, "");
        text = text.slice(end + 1);
        if (line.startsWith(prefix)) {success = true; return line;}
        if (line === "ERROR") throw new Error("Module rejected maintenance entry");
      }
      pendingRead = reader.read();
    }
  } finally {
    clearTimeout(timer); clearTimeout(retryTimer);
    signal?.removeEventListener("abort", abort);
    if (!success) try {await reader.cancel();} catch {}
    reader.releaseLock();
  }
}
export async function flashViaRadio({
  serial = navigator.serial, board, profile, family, signal,
  status = () => {}, progress = () => {}, fetcher = fetch,
  sha256 = async data => new Uint8Array(await crypto.subtle.digest("SHA-256", data)),
  loadDependencies = defaultDependencies, lineReader = readBridgeLine,
  settle = ms => new Promise(resolve => setTimeout(resolve, ms))
}) {
  let port, transport, abortPort, opened = false, verified = false;
  const check = () => {if (signal?.aborted) throw new Error("Canceled");};
  const json = async url => {
    const r = await fetcher(url, {signal, cache:"no-store"});
    if (!r.ok) throw new Error("Firmware manifest unavailable");
    return r.json();
  };
  try {
    check();
    // requestPort remains in the button's user-gesture call chain.
    port = await serial.requestPort();
    check();
    const base = "./" + board + "/" + profile + "/";
    const meta = await json(base + "full.json");
    if (!/^[a-z0-9_.-]+\.bin$/i.test(meta.path)) throw new Error("Invalid firmware path");
    status("Downloading and checking the full firmware image...");
    const response = await fetcher(base + meta.path, {signal, cache:"no-store"});
    if (!response.ok) throw new Error("Firmware image unavailable");
    const data = new Uint8Array(await response.arrayBuffer());
    await verifyFullImage(meta, data, board, profile, family, sha256);
    const {ESPLoader, Transport, md5} = await loadDependencies();
    check();
    await port.open({baudRate:115200, bufferSize:4096}); opened = true;
    await port.setSignals({dataTerminalReady:true, requestToSend:false});
    abortPort = () => port.setSignals({dataTerminalReady:false, requestToSend:false}).catch(() => {});
    signal?.addEventListener("abort", abortPort, {once:true});
    status("Waiting for the radio's USB maintenance mode...");
    // The host can open while radio AT preparation is still finishing.
    // STATUS does not establish binary forwarding, so it is safe to retry.
    const ready = port.writable.getWriter();
    try {await ready.write(new TextEncoder().encode("AT+BRIDGE=STATUS?\r\n"));}
    finally {ready.releaseLock();}
    await lineReader(port, "OK+BRIDGE:STATUS,", signal, 8000, "AT+BRIDGE=STATUS?\r\n");
    check();
    status("Checking radio USB maintenance authorization...");
    // The OS may assert DTR before binary terminal setup. Query only after
    // open/signals have completed; unsolicited banners can be echoed by tty.
    const probe = port.writable.getWriter();
    try {await probe.write(new TextEncoder().encode("AT+BRIDGE?\r\n"));}
    finally {probe.releaseLock();}
    const banner = await lineReader(port, "OK+BRIDGE:", signal);
    if (!/^OK\+BRIDGE:2,115200,(READY|RECOVERY)$/.test(banner))
      throw new Error("Radio USB bridge requires updated EdgeTX firmware");
    check();
    if (banner.endsWith(",READY")) {
      const writer = port.writable.getWriter();
      try {await writer.write(new TextEncoder().encode("AT+DOWNLOAD\r\n"));}
      finally {writer.releaseLock();}
      const ack = await lineReader(port, "OK+DOWNLOAD", signal);
      if (ack !== "OK+DOWNLOAD") throw new Error("ROM entry was not acknowledged");
    }
    check();
    // Reuse the authorized open port. ESPLoader normally opens it again;
    // reopening would drop DTR and terminate this maintenance connection.
    class RadioTransport extends Transport {
      async connect(baud) {
        if (baud !== 115200) throw new Error("Radio bridge requires 115200 baud");
        if (!this.device.readable) await super.connect(baud);
        this.baudrate = baud;
      }
    }
    transport = new RadioTransport(port, false);
    const loader = new ESPLoader({
      transport, baudrate:115200, romBaudrate:115200,
      terminal:{writeLine:status, write:status, clean:() => {}}
    });
    await loader.connect("no_reset");
    if (loader.chip.CHIP_NAME !== family)
      throw new Error("Detected chip does not match the selected board");
    if (loader.chip.IMAGE_CHIP_ID !== undefined &&
        loader.chip.IMAGE_CHIP_ID !== meta.chipId)
      throw new Error("Firmware chip ID does not match the connected chip");
    check();
    await loader.flashSpiAttach(0);
    status("Flashing through the radio. Keep USB connected...");
    await loader.writeFlash({
      fileArray:[{data, address:0}], flashSize:"keep",
      flashMode:"keep", flashFreq:"keep", eraseAll:false, compress:true,
      reportProgress:(_index, written, total) => {check(); progress(written / total);}
    });
    check();
    // Explicit ROM readback MD5, in addition to the downloaded SHA-256.
    const actual = normalizeRomMd5(await loader.flashMd5sum(0, data.length));
    if (actual !== hex(md5(data))) throw new Error("Flash readback checksum mismatch");
    check();
    verified = true;
    status("Flash contents verified. Requesting the module watchdog restart...");
    await restartRadioModule(loader, family, settle);
    check();
    // Windows usbser may not propagate RTS-only changes. Reopen the same
    // authorized port for an acknowledged bridge-management command instead.
    // No management text is injected into the binary ROM stream.
    await port.setSignals({dataTerminalReady:false, requestToSend:false});
    await transport.disconnect(); transport = null; opened = false;
    check();
    await port.open({baudRate:115200, bufferSize:4096}); opened = true;
    await port.setSignals({dataTerminalReady:true, requestToSend:false});
    check();
    const finish = port.writable.getWriter();
    try {await finish.write(new TextEncoder().encode("AT+BRIDGE=FINISH\r\n"));}
    finally {finish.releaseLock();}
    const finishAck = await lineReader(port, "OK+BRIDGE:FINISH", signal);
    if (finishAck !== "OK+BRIDGE:FINISH")
      throw new Error("Radio did not acknowledge update completion");
    check();
    await settle(150);
    status("Flash verified. Check the radio for confirmed module restart and Bluetooth restoration.");
    return {verified:true, restartRequested:true, board, profile, version:meta.version};
  } catch (e) {
    status((verified ? "Flash verified, restart not confirmed: " : "Update not confirmed: ") +
      e.message + " Reconnect the installer to retry if the radio shows recovery.");
    throw e;
  } finally {
    signal?.removeEventListener("abort", abortPort);
    if (opened) {
      try {await port.setSignals({dataTerminalReady:false, requestToSend:false});} catch {}
      if (transport) {
        try {await transport.disconnect();} catch {}
      } else {
        try {await port.close();} catch {}
      }
    }
  }
}
