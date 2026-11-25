import test from "node:test";
import assert from "node:assert/strict";
import {createHash} from "node:crypto";
import {flashViaRadio, normalizeRomMd5, readBridgeLine, restartRadioModule} from "../installer/uart-flasher.mjs";

function fixture(options = {}) {
  const data = new Uint8Array(0x10000+64);
  data[0x2000]=0xe9;data[0x8000]=0xaa;data[0x8001]=0x50;
  data[0x10000]=0xe9;data[0x1000c]=23;
  const hash = algo => new Uint8Array(createHash(algo).update(data).digest());
  const hex = bytes => Buffer.from(bytes).toString("hex");
  const meta = {board:"c5",profile:"production",chipFamily:"ESP32-C5",version:"test",
    address:0,bootOffset:0x2000,chipId:23,path:"firmware.bin",size:data.length,
    sha256:hex(hash("sha256")), ...options.meta};
  const calls=[];
  const port = {readable:null, writable:{getWriter:() => ({
    write:async bytes => calls.push(["download",new TextDecoder().decode(bytes)]),
    releaseLock:() => {}
  })},
    open:async o => {calls.push(["open",o]);port.readable={};},
    close:async () => {calls.push(["close"]);port.readable=null;},
    setSignals:async s => calls.push(["signals",s])
  };
  class Transport {
    constructor(device) {this.device=device;}
    async connect() {throw new Error("Must reuse authorized port");}
    async disconnect() {await port.close();}
  }
  class ESPLoader {
    constructor({transport,baudrate,romBaudrate}) {
      assert.equal(baudrate,115200);assert.equal(romBaudrate,115200);
      this.transport=transport;
      this.chip={CHIP_NAME:options.chip || "ESP32-C5",IMAGE_CHIP_ID:23};
    }
    async connect(mode) {calls.push(["connect",mode]);await this.transport.connect(115200);}
    async flashSpiAttach() {calls.push(["attach"]);}
    async writeFlash(o) {
      calls.push(["flash"]);assert.deepEqual(o.fileArray[0].data,data);
      assert.equal(o.eraseAll,false);assert.equal(o.compress,true);
      if(options.abortDuringFlash) options.abortDuringFlash();
      o.reportProgress(0,1,1);
    }
    async flashMd5sum() {
      calls.push(["md5"]);return options.badMd5 ? "0".repeat(32) :
        Buffer.from(hex(hash("md5"))).toString("hex"); // ROM ASCII checksum
    }
    async writeReg(address, value) {
      calls.push(["reset",address,value]);
      if(options.resetFails) throw new Error("reset failed");
    }
  }
  const args = {serial:{requestPort:async () => port},board:"c5",
    profile:"production",family:"ESP32-C5",
    fetcher:async url => ({ok:true,json:async () => meta,arrayBuffer:async () => data.buffer}),
    sha256:async () => hash("sha256"),
    loadDependencies:async () => ({Transport,ESPLoader,md5:() => hash("md5")}),
    lineReader:async (_p,prefix) => {
      if (prefix === "OK+BRIDGE:") return "OK+BRIDGE:"+(options.legacy?1:2)+
        ",115200,"+(options.recovery?"RECOVERY":"READY");
      if (prefix === "OK+BRIDGE:FINISH") {
        if (options.finishFails) throw new Error("completion acknowledgment timed out");
        return options.badFinishAck ? "OK+BRIDGE:FINISH_EXTRA" : "OK+BRIDGE:FINISH";
      }
      return "OK+DOWNLOAD";
    },
    settle:async () => {}
  };
  return {calls,args,port};
}
test("authorized ROM flow checks image, chip, flash readback, then watchdog restart",async () => {
  const {args,calls}=fixture();const result=await flashViaRadio(args);
  assert.equal(result.verified,true);
  const probe=calls.findIndex(x=>x[0]==="download"&&x[1]==="AT+BRIDGE?\r\n");
  const download=calls.findIndex(x=>x[0]==="download"&&x[1]==="AT+DOWNLOAD\r\n");
  assert(probe>calls.findIndex(x=>x[0]==="signals")&&probe<download);
  assert.equal(calls.filter(x=>x[0]==="open").length,2);
  assert.deepEqual(calls.find(x=>x[0]==="connect"),["connect","no_reset"]);
  assert.deepEqual(calls.find(x=>x[0]==="download"&&x[1]==="AT+DOWNLOAD\r\n"),["download","AT+DOWNLOAD\r\n"]);
  assert(calls.findIndex(x=>x[0]==="md5")<calls.findIndex(x=>x[0]==="reset"));
  assert.equal(result.restartRequested,true);
  const finish=calls.findIndex(x=>x[0]==="download"&&x[1]==="AT+BRIDGE=FINISH\r\n");
  const reopen=calls.findLastIndex(x=>x[0]==="open");
  assert(reopen>calls.findLastIndex(x=>x[0]==="reset")&&finish>reopen);
  assert(calls.findIndex(x=>x[0]==="close")<reopen);
  assert(!calls.some(x=>x[0]==="signals"&&x[1].requestToSend===true));
  assert.equal(calls.at(-1)[0],"close");
});
test("ROM recovery reconnect skips AT download",async () => {
  const {args,calls}=fixture({recovery:true});await flashViaRadio(args);
  assert(!calls.some(x=>x[0]==="download"&&x[1]==="AT+DOWNLOAD\r\n"));
});
test("wrong chip prevents erase/write",async () => {
  const {args,calls}=fixture({chip:"ESP32-C3"});
  await assert.rejects(flashViaRadio(args),/Detected chip/);
  assert(!calls.some(x=>x[0]==="flash"));
});
test("wrong manifest and corrupt SHA prevent opening the maintenance port",async () => {
  for(const meta of [{board:"c3-supermini"},{sha256:"0".repeat(64)},{path:"../other.bin"}]) {
    const {args,calls}=fixture({meta});await assert.rejects(flashViaRadio(args));
    assert(!calls.some(x=>x[0]==="open"||x[0]==="flash"));
  }
});
test("failed flash readback never requests completion or reset",async () => {
  const {args,calls}=fixture({badMd5:true});
  await assert.rejects(flashViaRadio(args),/checksum mismatch/);
  assert(!calls.some(x=>x[0]==="reset"||(x[0]==="download"&&x[1]==="AT+BRIDGE=FINISH\r\n")));
});
test("restart failure is not reported as successful completion",async () => {
  const {args,calls}=fixture({resetFails:true});
  await assert.rejects(flashViaRadio(args),/reset failed/);
  assert(!calls.some(x=>x[0]==="download"&&x[1]==="AT+BRIDGE=FINISH\r\n"));
});
test("cancel during flash drops DTR and does not request completion",async () => {
  const controller=new AbortController();
  const {args,calls}=fixture({abortDuringFlash:() => controller.abort()});
  await assert.rejects(flashViaRadio({...args,signal:controller.signal}),/Canceled/);
  assert(!calls.some(x=>x[0]==="reset"||(x[0]==="download"&&x[1]==="AT+BRIDGE=FINISH\r\n")));
  assert(calls.some(x=>x[0]==="signals"&&x[1].dataTerminalReady===false));
});
test("ROM checksum parser rejects invalid acknowledgements",() => {
  assert.equal(normalizeRomMd5("A".repeat(32)),"a".repeat(32));
  assert.throws(() => normalizeRomMd5("complete"),/Invalid/);
});
test("serial acknowledgement parser handles split packets and releases the lock",async () => {
  const stream=new ReadableStream({start(c) {
    for(const s of ["noise\r\nOK+BR","IDGE:1,115200,READY\r\n"])c.enqueue(new TextEncoder().encode(s));
  }});
  assert.equal(await readBridgeLine({readable:stream},"OK+BRIDGE:",undefined,100),
    "OK+BRIDGE:1,115200,READY");
  assert.equal(stream.locked,false);
});
test("missing acknowledgement times out and releases the reader",async () => {
  const stream=new ReadableStream({});
  await assert.rejects(readBridgeLine({readable:stream},"OK+BRIDGE:",undefined,5));
  assert.equal(stream.locked,false);
});

test("C3/C5 watchdog restart uses each target's maintained ROM reset registers",async () => {
  for (const [family,base,protect] of [
    ["ESP32-C3",0x60008090,0x600080a8],
    ["ESP32-C5",0x600b1c00,0x600b1c18]
  ]) {
    const writes=[],waits=[];
    await restartRadioModule({writeReg:async (...a)=>writes.push(a)},family,async n=>waits.push(n));
    assert.deepEqual(writes,[[protect,0x50d83aa1],[base+4,2000],[base,0xd0000102],[protect,0]]);
    assert.deepEqual(waits,[500]);
  }
  await assert.rejects(restartRadioModule({},"ESP32",async()=>{}),/Unsupported/);
});


test("legacy bridge is rejected before ROM entry or flash",async () => {
  const {args,calls}=fixture({legacy:true});
  await assert.rejects(flashViaRadio(args),/updated EdgeTX/);
  assert(!calls.some(x=>x[0]==="flash"||(x[0]==="download"&&x[1]==="AT+DOWNLOAD\r\n")));
});
test("missing or invalid completion acknowledgment never reports success",async () => {
  for (const option of [{finishFails:true},{badFinishAck:true}]) {
    const {args,calls}=fixture(option);
    await assert.rejects(flashViaRadio(args),/completion/);
    assert.equal(calls.at(-1)[0],"close");
  }
});
