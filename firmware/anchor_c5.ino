#include <Arduino.h>
#include <BLE.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_app_desc.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ESP32-C5 CC2540 UART shim: TX15 PB9 -> GPIO12 (RX), PB8 <- GPIO11 (TX).
// FrSky BLE GATT protocol from Cliff BTWifiModule: 0xFFF0 / 0xFFF6 notify.
// This build deliberately does NOT simulate connections or invent trainer data.
static const int RX_PIN=12,TX_PIN=11, EN_MONITOR_PIN=1; // XIAO D0/GPIO1; INPUT ONLY; deep-sleep wake candidate
static const BLEUUID SERVICE_UUID("FFF0"), TRAINER_UUID("FFF6");
static BLEClient client;
// Peripheral mode for EdgeTX BLUETOOTH_TELEMETRY (AT+ROLE0).
// Raw EdgeTX UART stream is transported losslessly as ordered GATT notifications.
// BLE-only; not Bluetooth Classic SPP.
static bool telemetryRole=false, telemetryForcePending=false;
static bool anchorOtaPending=false,anchorOtaActive=false,anchorOtaUploading=false;
static bool anchorOtaSuccess=false,anchorOtaHeaderValid=false;
static uint32_t anchorOtaLastActivity=0,anchorOtaRestartAt=0;
static uint32_t anchorOtaDisableHighSince=0;
static bool anchorOtaCancelDeferred=false;
static WebServer anchorOtaWeb(80);
static constexpr uint32_t ANCHOR_OTA_IDLE_MS=600000;
static const char ANCHOR_OTA_SSID[]="ANCHOR Chain";
static const char ANCHOR_OTA_PASS[]="anchor-update";
static bool persistedTelemetryRole=false;
// EdgeTX supplies its configured Bluetooth name with AT+NAME.
// Retain it across BLE restarts and C5-only reboots.
static String radioBluetoothName = "ANCHOR";

static BLEServer telemetryServer;
static BLECharacteristic telemetryNotify;
static BLECharacteristic telemetryWrite;
static volatile bool telemetryConnected=false, telemetryNotifyConnect=false, telemetryNotifyDisconnect=false;
static volatile bool telemetryStartAdvertising=false;
static String telemetryPeer;
static uint8_t telemetryBytes[4096];
static size_t telemetryHead=0,telemetryTail=0;
static uint32_t telemetryUartBytes=0,telemetryBleBytes=0,telemetryBlePackets=0;
static uint32_t telemetryDrops=0,telemetryNotifyBusy=0,telemetryMaxDepth=0;
static uint32_t telemetryLastSentMs=0,telemetryFirstPendingUs=0,telemetryNoSubscriberDrops=0,telemetrySendFailures=0;
static const BLEUUID TELEMETRY_NOTIFY_UUID("FFF6"), TELEMETRY_WRITE_UUID("FFF7");
static size_t telemetryDepth(){return (telemetryHead+sizeof(telemetryBytes)-telemetryTail)%sizeof(telemetryBytes);}
static void telemetryPush(uint8_t value){
 if(telemetryHead==telemetryTail)telemetryFirstPendingUs=micros();
 size_t next=(telemetryHead+1)%sizeof(telemetryBytes);
 if(next==telemetryTail){++telemetryDrops;return;}
 telemetryBytes[telemetryHead]=value;telemetryHead=next;++telemetryUartBytes;
 size_t depth=telemetryDepth();if(depth>telemetryMaxDepth)telemetryMaxDepth=depth;
}

static Preferences prefs;
static BLEScan::Results lastScan;
static QueueHandle_t packetQueue;
struct Packet { uint8_t len; uint8_t data[64]; uint32_t receivedUs; };
static volatile uint32_t notifications=0, dropped=0, superseded=0, lastPacketMs=0;
// End-to-end profiler: USB STATUS snapshot is sampled without serial printing
// in BLE callbacks, to avoid perturbing notification timing.
static uint32_t uartPackets=0, uartBytes=0, uartWriteMaxUs=0;
static uint32_t maxQueueDepth=0, maxRxToUartUs=0, totalRxToUartUs=0;
static uint32_t mainLoops=0, mainGapMaxUs=0, mainGapsOver10ms=0;
static uint32_t mainLastUs=0;

static volatile uint32_t maxGapMs=0, gapsOver100Ms=0, malformedNotifications=0;
static volatile uint32_t linkStalls=0;
// Worker is the sole owner of blocking connection/GATT operations.
// Main task remains responsible for PH11, UART and BLE lifecycle.
static volatile bool workerActive=false,workerDone=false,workerCancel=false,workerSuccess=false;
static volatile uint32_t workerStartedMs=0,connectFailures=0,connectCancels=0;
static String workerPeer;
static uint8_t workerAddrType=0;
static bool deferredRadioOff=false;
static uint32_t nextStallCheck=0;

static bool bleReady=false, subscribed=false, requestScan=false, requestConnect=false, simulate=false;
static bool autoReconnectPending=false, connectedAnnounced=false;
// Matched low-latency HT transport: legacy AT handshake, modern BLE link.
// Tune after the connect worker exits; NEVER call GATT operations inside a
// notification callback and never hold up EdgeTX UART initialization.
static uint8_t linkTuneStage=0; // 0 idle, 1 request interval, 2 request PHY
static uint32_t linkTuneAt=0, linkTuneSuccess=0, linkTuneErrors=0;

static String selectedPeer, savedPeer, currentPeer;
static uint8_t savedAddrType=0;
static String uartLine, usbLine;
static uint32_t uartBaud=57600, retryAt=0, lastLog=0, bootAt=0;
static int enLast=-1; static uint32_t enTransitions=0, enChangedAt=0;
static bool awaitingHostInit=true;
static bool enEnabled=false, bleStartPending=false, fallbackAllowed=false;
static int enCandidate=-1;
static uint32_t enCandidateAt=0;
static uint32_t bleStarts=0, bleStops=0, bleStartAt=0;
static uint32_t offSince=0; static bool sleepAttempted=false;
static bool deepSleepWake=false, wakeRecoveryPending=false;
static uint32_t wakeRecoveryAt=0, recoveryAttempts=0;
static const uint32_t DEEP_SLEEP_HIGH_MS=2500;
// EdgeTX has no timeout in NAME_SENT / POWER_SENT. The C5 must track
// commands received, not assume that a PH11 cycle means setup succeeded.
// Stages reset on every PH11 transition. Never reset a completed handshake.
enum InitStage : uint8_t { INIT_WAIT_BAUD=0, INIT_WAIT_NAME=1,
  INIT_WAIT_POWER=2, INIT_WAIT_ROLE=3, INIT_COMPLETE=4 };
static InitStage initStage=INIT_WAIT_BAUD;
static uint32_t initStageAt=0, initCycleAt=0, initResyncAt=0;
static uint32_t initResyncCount=0, initLastResetAt=0;
static uint8_t initRetryBudgetUsed=0;
static const uint8_t INIT_MAX_RETRIES=3;
static bool initResetPending=false, initBootFallbackActive=false;
static const uint32_t INIT_STAGE_TIMEOUT_MS=4000;
static const uint32_t INIT_FIRST_BAUD_TIMEOUT_MS=4000;
static const uint32_t INIT_RECOVERY_COOLDOWN_MS=12000;
static void initStart(){
 initBootFallbackActive=false;
 initStage=INIT_WAIT_BAUD;initStageAt=initCycleAt=millis();
 // Keep a pending reset across a PH11 cycle only until it has completed.
 if(initResetPending) initResetPending=false;
}
static void initAdvance(int st){
 if(st>initStage){initStage=(InitStage)st;initStageAt=millis();}
}

static uint32_t roleAckAt=0; static uint8_t roleAckRemaining=0;
// Cooperative 3.5-second BLE scan: polling stays responsive to PH11 and UART.
static bool scanActive=false, scanReportUART=false, scanRequestReportUART=false;
static String scanRequestedPeer;
static uint32_t scanDeadline=0, scanStarts=0, scanCompletes=0, scanCancels=0;


static String canonical(String a){a.toUpperCase();a.replace(":","");a.replace("-","");return a;}
static bool validAddr(String a){if(a.length()!=12)return false;for(char c:a)if(!isxdigit((unsigned char)c))return false;return true;}
// The actual ESP32-C5 public Bluetooth MAC can be read before BLE.begin().
// This is required because EdgeTX requests its identity before AT+ROLE1,
// and BLE is intentionally initialized only after the UART handshake.
static String localBtAddr;
static void readLocalBtAddr(){
 uint8_t mac[6]={};
 if(esp_read_mac(mac,ESP_MAC_BT)==ESP_OK){
  char str[13];snprintf(str,sizeof(str),"%02X%02X%02X%02X%02X%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  localBtAddr=String(str);
 }
 Serial.printf("LOCAL_BT_ADDR %s\n",localBtAddr.c_str());
}
static void reply(const String& s){Serial1.print(s);Serial1.print("\r\n");Serial.printf("UART< %s\n",s.c_str());}
static void requestEdgeTxReset(){
 // A stray byte may precede us when EdgeTX switches UART baud. Its
 // readline() demands an exact ERROR line. A leading empty CRLF clears
 // any partial line on the EdgeTX parser before the reset signal.
 Serial1.print("\r\nERROR\r\n");Serial1.flush();
 Serial.println("UART< <CRLF>ERROR<CRLF> clean parser recovery request");
}
static void replyPeripheral(){
 if(validAddr(localBtAddr))reply("Peripheral:"+localBtAddr);
 else Serial.println("LOCAL_BT_ADDR unavailable; refusing placeholder");
}
static String asMac(const BTAddress& address){
 String s=address.toString();
 s.replace(":","");s.replace("-","");s.toUpperCase();return s;
}
static void telemetryStartServer(){
 telemetryServer=BLE.createServer();
 if(!telemetryServer){Serial.println("TELEMETRY_SERVER_ERROR create");return;}
 telemetryServer.onConnect([](BLEServer&,const BLEConnInfo& conn){
   telemetryPeer=asMac(conn.getAddress());
   telemetryConnected=true;telemetryNotifyConnect=true;
 });
 telemetryServer.onDisconnect([](BLEServer&,const BLEConnInfo&,uint8_t){
   telemetryConnected=false;telemetryNotifyDisconnect=true;telemetryStartAdvertising=true;
 });
 BLEService svc=telemetryServer.createService(SERVICE_UUID);
 if(!svc){Serial.println("TELEMETRY_SERVICE_ERROR");return;}
 telemetryNotify=svc.createCharacteristic(TELEMETRY_NOTIFY_UUID,BLEProperty::Read | BLEProperty::Notify,BLEPermission::ReadOpen);
 telemetryWrite=svc.createCharacteristic(TELEMETRY_WRITE_UUID,BLEProperty::Write,BLEPermission::WriteOpen);
 if(!telemetryNotify || !telemetryWrite){Serial.println("TELEMETRY_CHARACTERISTIC_ERROR");return;}
 telemetryNotify.setValue((const uint8_t*)"",1);
 // UART input is unused by EdgeTX in telemetry mode. Reserve FFF7 for future
 // bidirectional support; never inject BLE writes into EdgeTX control/telemetry.
 BTStatus start=telemetryServer.start();
 if(!start){Serial.printf("TELEMETRY_SERVER_START_FAILED %s\n",start.toString());return;}
 BLEAdvertising adv=BLE.getAdvertising();adv.addServiceUUID(SERVICE_UUID);
 BTStatus status=adv.start();
 Serial.printf("TELEMETRY_ADVERTISING name=%s status=%s\n",radioBluetoothName.c_str(),status.toString());
}
// TelemetryViewer Android 1.9.x only recognizes CRSF RADIO_ADDRESS 0xEA,
// although EdgeTX emits valid telemetry with standard CRSF sync 0xC8.
// Convert ONLY the address byte of complete CRC-verified frames on BLE.
// CRSF CRC covers type+payload, NOT the address; all other bytes are unchanged.
// USB switch permits opt-out for other BLE serial clients.
static bool telemetryViewerCompat=true;
static uint8_t telemetryCrsfPending[64]={};
static uint8_t telemetryCrsfCount=0;
static uint32_t telemetryCrsfConverted=0,telemetryCrsfInvalid=0;
static uint8_t telemetryCrsfCrc(const uint8_t* data,size_t count){
 uint8_t crc=0;
 for(size_t i=0;i<count;i++){
   crc^=data[i];
   for(uint8_t bit=0;bit<8;bit++)
     crc=(crc&0x80)?(uint8_t)((crc<<1)^0xD5):(uint8_t)(crc<<1);
 }
 return crc;
}
static void telemetryInputByte(uint8_t byte){
 if(!telemetryViewerCompat){telemetryPush(byte);return;}
 if(telemetryCrsfCount==0){
   if(byte!=0xC8){telemetryPush(byte);return;}
   telemetryCrsfPending[telemetryCrsfCount++]=byte;return;
 }
 telemetryCrsfPending[telemetryCrsfCount++]=byte;
 if(telemetryCrsfCount==2 && (byte<2 || byte>62)){
   for(uint8_t i=0;i<telemetryCrsfCount;i++)telemetryPush(telemetryCrsfPending[i]);
   telemetryCrsfCount=0; ++telemetryCrsfInvalid;return;
 }
 if(telemetryCrsfCount>=2 && telemetryCrsfCount==(uint8_t)(telemetryCrsfPending[1]+2)){
   bool valid=(telemetryCrsfCrc(&telemetryCrsfPending[2],telemetryCrsfCount-3)==telemetryCrsfPending[telemetryCrsfCount-1]);
   if(valid){telemetryCrsfPending[0]=0xEA;++telemetryCrsfConverted;}
   else ++telemetryCrsfInvalid;
   for(uint8_t i=0;i<telemetryCrsfCount;i++)telemetryPush(telemetryCrsfPending[i]);
   telemetryCrsfCount=0;
 }
}
static void telemetryDrain(){
 if(!telemetryConnected || !telemetryNotify || telemetryNotify.getSubscribedCount()==0){
   // UART telemetry has no durable consumer; do not build unbounded old-data.
   telemetryNoSubscriberDrops+=telemetryDepth();telemetryTail=telemetryHead;
   return;
 }
 if(telemetryHead==telemetryTail)return;
 const size_t available=telemetryDepth();
 // Coalesce a short UART burst, preserving raw byte order and frame boundaries
 // implicitly in the byte stream. Max intentional 1.8ms delay.
 if(available<20 && (uint32_t)(micros()-telemetryFirstPendingUs)<1800)return;
 const size_t count=available<20?available:20;  // works at BLE MTU=23
 uint8_t payload[20];
 for(size_t i=0;i<count;i++)payload[i]=telemetryBytes[(telemetryTail+i)%sizeof(telemetryBytes)];
 const BTStatus result=telemetryNotify.notify(payload,count);
 if(result){
  telemetryTail=(telemetryTail+count)%sizeof(telemetryBytes);
  if(telemetryHead!=telemetryTail)telemetryFirstPendingUs=micros();
  telemetryBleBytes+=count;++telemetryBlePackets;telemetryLastSentMs=millis();
 }else{++telemetryNotifyBusy;++telemetrySendFailures;}
}
static void replyCentral(){
 if(validAddr(localBtAddr))reply("Central:"+localBtAddr);
 else Serial.println("LOCAL_BT_ADDR unavailable; refusing placeholder");
}
static void uartSwitch(uint32_t baud){Serial1.flush();Serial1.end();Serial1.begin(baud,SERIAL_8N1,RX_PIN,TX_PIN);uartBaud=baud;Serial.printf("UART baud=%lu\n",(unsigned long)baud);}
static void onNotify(BLERemoteCharacteristic, const uint8_t* data, size_t len, BLESubscription){
 // Preserve exactly the bytes sent by HT. The original HT firmware writes
 // its checksum byte unescaped (even if it equals 0x7E or 0x7D). EdgeTX
 // handles framing on the UART; the shim must not drop these packets.
 if(!packetQueue || !data || len==0 || len>64){++malformedNotifications;++dropped;return;}
 Packet p;p.len=(uint8_t)len;memcpy(p.data,data,len);p.receivedUs=micros();
 uint32_t now=millis(), prior=lastPacketMs;
 // BLE can deliver several notifications in one connection event. Keep a
 // small bounded burst buffer so the fast UART loop can drain them after the
 // callback yields. Evict oldest only if all four slots are occupied.
 uint32_t depth=(uint32_t)uxQueueMessagesWaiting(packetQueue);
 if(depth+1>maxQueueDepth)maxQueueDepth=depth+1;
 if(xQueueSend(packetQueue,&p,0)!=pdTRUE){
   Packet oldest;
   xQueueReceive(packetQueue,&oldest,0);
   ++superseded;
   xQueueSend(packetQueue,&p,0);
 }
 if(prior){int32_t gap=(int32_t)(now-prior);if(gap>0){if((uint32_t)gap>maxGapMs)maxGapMs=gap;if(gap>100)++gapsOver100Ms;}}
 ++notifications;
 __atomic_store_n(&lastPacketMs,now,__ATOMIC_RELEASE);
}
static void clearConnection(bool emitLost){
 linkTuneStage=0;
 bool old=subscribed;subscribed=false;connectedAnnounced=false;currentPeer="";
 if(workerActive){workerCancel=true;client.cancelConnect();}
 else if(client && client.isConnected())client.disconnect();
 if(emitLost && old){reply("DisConnected");Serial.println("BLE disconnected");}
 if(packetQueue)xQueueReset(packetQueue);
 lastPacketMs=0;
}
static void pollWorker();

static bool anchorAuthorized(){
  if(anchorOtaWeb.authenticate("admin",ANCHOR_OTA_PASS)) return true;
  anchorOtaWeb.requestAuthentication();
  return false;
}
static void anchorOtaRoutes(){
  anchorOtaWeb.on("/",HTTP_GET,[](){
    if(!anchorAuthorized())return;
    anchorOtaLastActivity=millis();
    anchorOtaWeb.send(200,"text/html",
      "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>ANCHOR Chain</title><h1>ANCHOR Chain</h1>"
      "<p>Upload an ANCHOR ESP32-C5 application .bin file, not a merged flash image.</p>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='firmware' accept='.bin' required>"
      "<button type='submit'>Install firmware</button></form>"
      "<p>Leave the TX15 powered on during the update.</p>");
  });
  anchorOtaWeb.on("/update",HTTP_POST,[](){
    if(!anchorAuthorized())return;
    bool ok=anchorOtaSuccess && !Update.hasError();
    anchorOtaWeb.send(ok?200:400,"text/plain",
      ok?"Update verified. ANCHOR will restart.":"Firmware upload failed; existing firmware retained.");
    anchorOtaUploading=false;
    if(ok)anchorOtaRestartAt=millis()+1500;
  },[](){
    if(!anchorOtaWeb.authenticate("admin",ANCHOR_OTA_PASS))return;
    HTTPUpload& upload=anchorOtaWeb.upload();
    anchorOtaLastActivity=millis();
    if(upload.status==UPLOAD_FILE_START){
      anchorOtaSuccess=false;
      anchorOtaUploading=false;
      anchorOtaHeaderValid=false;
      // Arduino Update writes only to the inactive OTA application slot.
      // Explicit .bin check rejects mistakenly uploading a full merged image.
      if(!upload.filename.endsWith(".bin") ||
         upload.filename.indexOf("merged")>=0 ||
         !Update.begin(UPDATE_SIZE_UNKNOWN,U_FLASH)) {
        Serial.println("ANCHOR_OTA_START rejected");
        return;
      }
      anchorOtaUploading=true;
      Serial.printf("ANCHOR_OTA_START file=%s\n",upload.filename.c_str());
    }else if(upload.status==UPLOAD_FILE_WRITE){
      if(anchorOtaUploading && !anchorOtaHeaderValid){
        uint32_t descMagic=0;
        if(upload.currentSize>=36)
          memcpy(&descMagic,upload.buf+32,sizeof(descMagic));
        // Application binary: ESP image header plus app description at
        // offset 32. A merged flash image/bootloader does not qualify.
        if(upload.currentSize<36 || upload.buf[0]!=0xE9 ||
           descMagic!=ESP_APP_DESC_MAGIC_WORD){
          Serial.println("ANCHOR_OTA_BAD_IMAGE not an application binary");
          Update.abort();anchorOtaUploading=false;
        }else anchorOtaHeaderValid=true;
      }
      if(anchorOtaUploading &&
         Update.write(upload.buf,upload.currentSize)!=upload.currentSize){
        Serial.println("ANCHOR_OTA_WRITE failed");
        Update.abort();anchorOtaUploading=false;
      }
    }else if(upload.status==UPLOAD_FILE_END){
      if(anchorOtaUploading && Update.end(true)){
        anchorOtaSuccess=true;
        Serial.printf("ANCHOR_OTA_VERIFIED bytes=%u\n",(unsigned)upload.totalSize);
      }else {
        Update.abort();
        Serial.println("ANCHOR_OTA_VERIFY failed");
      }
    }else if(upload.status==UPLOAD_FILE_ABORTED){
      Update.abort();anchorOtaUploading=false;
      Serial.println("ANCHOR_OTA_ABORTED");
    }
  });
  anchorOtaWeb.onNotFound([](){
    if(!anchorAuthorized())return;
    anchorOtaWeb.send(404,"text/plain","Not found");
  });
}
static void anchorOtaBegin(){
  if(anchorOtaActive)return;
  // The radio must keep the adapter powered; BLE is inactive during update.
  autoReconnectPending=false;requestScan=false;requestConnect=false;
  bleStartPending=false;roleAckRemaining=0;
  if(scanActive && bleReady){BLE.getScan().stop();scanActive=false;}
  if(bleReady){
    if(client && client.isConnected())client.disconnect();
    BLE.end(false);bleReady=false;++bleStops;
    client=BLEClient();
    telemetryServer=BLEServer();
    telemetryNotify=BLECharacteristic();
    telemetryWrite=BLECharacteristic();
  }
  subscribed=false;telemetryConnected=false;
  WiFi.mode(WIFI_AP);
  if(!WiFi.softAP(ANCHOR_OTA_SSID,ANCHOR_OTA_PASS)){
    Serial.println("ANCHOR_OTA_AP_FAILED restarting");
    ESP.restart();
    return;
  }
  anchorOtaRoutes();anchorOtaWeb.begin();
  anchorOtaActive=true;anchorOtaLastActivity=millis();
  Serial.printf("ANCHOR_OTA_READY ssid=%s ip=%s\n",ANCHOR_OTA_SSID,
    WiFi.softAPIP().toString().c_str());
}
static void anchorOtaPoll(){
  // The radio's module-enable signal is the generic out-of-band exit.
  // EdgeTX raises PH11 on Back from its OTA dialog for 1.8s.
  // Never interrupt a firmware flash: defer cancellation until upload
  // completes (successful uploads follow their normal reboot path).
  if(digitalRead(EN_MONITOR_PIN)==HIGH){
    if(!anchorOtaDisableHighSince)
      anchorOtaDisableHighSince=millis();
    if((uint32_t)(millis()-anchorOtaDisableHighSince)>=100){
      if(anchorOtaUploading){
        if(!anchorOtaCancelDeferred)
          Serial.println("ANCHOR_OTA_CANCEL deferred during upload");
        anchorOtaCancelDeferred=true;
      }else{
        Serial.println("ANCHOR_OTA_CANCEL PH11 HIGH; restart into Bluetooth");
        Serial.flush();
        ESP.restart();
      }
    }
  }else anchorOtaDisableHighSince=0;
  if(anchorOtaCancelDeferred && !anchorOtaUploading &&
     !anchorOtaRestartAt){
    Serial.println("ANCHOR_OTA_CANCEL deferred session complete");
    Serial.flush();
    ESP.restart();
  }
  if(anchorOtaPending && !anchorOtaActive){
    if(workerActive || workerDone){
      if(workerActive){workerCancel=true;client.cancelConnect();}
      pollWorker();
    }
    if(!workerActive){anchorOtaPending=false;anchorOtaBegin();}
    return;
  }
  if(!anchorOtaActive)return;
  anchorOtaWeb.handleClient();
  if(anchorOtaRestartAt && (int32_t)(millis()-anchorOtaRestartAt)>=0)
    ESP.restart();
  // A broken HTTP upload must not leave maintenance mode active forever.
  if((uint32_t)(millis()-anchorOtaLastActivity)>
       (anchorOtaUploading ? 120000UL : ANCHOR_OTA_IDLE_MS)){
    if(anchorOtaUploading){Update.abort();anchorOtaUploading=false;}
    Serial.println("ANCHOR_OTA_IDLE_TIMEOUT restarting");
    ESP.restart();
  }
}

// Treat PH11 as the authoritative Bluetooth enable. Keep BLE memory allocated
// so BLE.begin() can run after BLE.end(false) without resetting the C5.
static void radioOff(){
  // OTA is a deliberate maintenance session; do not sleep mid-flash.
  if(anchorOtaActive || anchorOtaPending)return;
  if(telemetryRole){
    telemetryConnected=false;telemetryNotifyConnect=false;telemetryNotifyDisconnect=false;
    telemetryStartAdvertising=false;telemetryHead=telemetryTail=0;
  }
  linkTuneStage=0;
  offSince=millis();sleepAttempted=false;
  if(workerActive){workerCancel=true;deferredRadioOff=true;client.cancelConnect();}
  bleStartPending=false;fallbackAllowed=false;roleAckRemaining=0;
  autoReconnectPending=false;requestScan=false;requestConnect=false;
  if(scanActive && bleReady){BLE.getScan().stop();++scanCancels;scanActive=false;scanRequestedPeer="";}
  subscribed=false;connectedAnnounced=false;currentPeer="";
  if(packetQueue)xQueueReset(packetQueue);
  lastPacketMs=0;
  if(bleReady && !workerActive){
    if(client && client.isConnected())client.disconnect();
    BLE.end(false);
    client=BLEClient();
    telemetryServer=BLEServer();
    telemetryNotify=BLECharacteristic();
    telemetryWrite=BLECharacteristic();
    lastScan=BLEScan::Results();
    bleReady=false;++bleStops;
  }
  uartLine="";
  telemetryRole=false;
  awaitingHostInit=true;
  initStage=INIT_WAIT_BAUD;initStageAt=0;
  if(uartBaud!=57600)uartSwitch(57600);
  Serial.printf("RADIO_OFF PH11=HIGH BLE=%d stops=%lu\n",bleReady,(unsigned long)bleStops);
}
static void radioOn(){
  if(anchorOtaActive || anchorOtaPending)return;
  offSince=0;sleepAttempted=false;
  // Any pending teardown completes before BLE is started again.
  // EdgeTX immediately transmits AT+BAUD4 after enabling PH11.
  // NEVER block here with BLE.begin() or a scan/connection.
  if(uartBaud!=57600)uartSwitch(57600);
  uartLine="";awaitingHostInit=true;fallbackAllowed=false;
  initStart();
  bleStartPending=false;roleAckRemaining=0;
  Serial.println("RADIO_ON PH11=LOW awaiting UART handshake at 57600");
}
static void startBLE(){
 if(!enEnabled || bleReady || workerActive || deferredRadioOff)return;
 bleStartPending=false;
   BTStatus result=BLE.begin(telemetryRole ? radioBluetoothName.c_str() : "ANCHOR");bleReady=(bool)result;
   if(bleReady){
     ++bleStarts;Serial.printf("BLE_STARTED role=%s count=%lu\n",telemetryRole?"peripheral":"central",(unsigned long)bleStarts);
     if(telemetryRole)telemetryStartServer();
   }
 else {Serial.printf("BLE_START_FAILED status=%s\n",result.toString());bleStartPending=true;bleStartAt=millis()+1500;}
}
static void readEnable(){
 int raw=digitalRead(EN_MONITOR_PIN);
 if(raw!=enCandidate){enCandidate=raw;enCandidateAt=millis();}
 if(raw!=enLast && (uint32_t)(millis()-enCandidateAt)>=50){
  enLast=raw;++enTransitions;enChangedAt=millis();
  Serial.printf("PH11_CHANGE level=%d transitions=%lu uptime=%lu\n",raw,(unsigned long)enTransitions,(unsigned long)enChangedAt);
  enEnabled=(raw==LOW);
  if(enEnabled)radioOn();else radioOff();
 }
}
// Asynchronous BLE discovery. Never block the UART/control loop while scanning.
static bool runScan(bool reportUART, const String &peer=String()){
 if(!enEnabled || !bleReady){if(reportUART)reply("ERROR");return false;}
 if(scanActive){
  // Keep the ongoing scan; a saved-peer reconnect can use its result too.
  if(validAddr(peer))scanRequestedPeer=peer;
  scanReportUART|=reportUART;
  return true;
 }
 BLEScan scan=BLE.getScan();scan.setActiveScan(true);scan.setInterval(100);scan.setWindow(70);
 BTStatus st=scan.start(3500);
 if(!st){Serial.printf("SCAN_START_FAILED %s\n",st.toString());if(reportUART)reply("ERROR");return false;}
 scanActive=true;scanReportUART=reportUART;scanRequestedPeer=peer;
 scanDeadline=millis()+4700;++scanStarts;
 Serial.printf("SCAN_START async peer=%s report=%d count=%lu\n",peer.c_str(),reportUART,(unsigned long)scanStarts);
 return true;
}
static void pollScan(){
 if(!scanActive || !bleReady)return;
 BLEScan scan=BLE.getScan();
 bool expired=(int32_t)(millis()-scanDeadline)>=0;
 if(scan.isScanning() && !expired)return;
 if(scan.isScanning()){
  scan.stop();++scanCancels;
  Serial.println("SCAN_TIMEOUT forcibly stopped");
 }
 scanActive=false;++scanCompletes;
 lastScan=scan.getResults();
 if(scanReportUART)reply("OK+DISCS");
 bool found=false;
 for(const BLEAdvertisedDevice &dev:lastScan){
  String addr=canonical(dev.getAddress().toString());
  if(!validAddr(addr))continue;
  if(scanReportUART)reply("OK+DISC:"+addr);
  Serial.printf("SCAN_DEVICE %s type=%d rssi=%d name=%s svc=%d\n",addr.c_str(),(int)dev.getAddressType(),dev.getRSSI(),dev.getName().c_str(),dev.isAdvertisingService(SERVICE_UUID));
  if(validAddr(scanRequestedPeer) && addr==scanRequestedPeer)found=true;
 }
 if(scanReportUART)reply("OK+DISCE");
 Serial.printf("SCAN_DONE count=%lu target=%s found=%d\n",(unsigned long)scanCompletes,scanRequestedPeer.c_str(),found);
 if(validAddr(scanRequestedPeer)){
  if(found){selectedPeer=scanRequestedPeer;requestConnect=true;}
  else {
   Serial.println("BLE peer not advertising; retry scheduled");
   if(validAddr(savedPeer) && savedPeer==scanRequestedPeer){retryAt=millis()+4000;autoReconnectPending=true;}
   else reply("ERROR");
  }
 }
 scanRequestedPeer="";scanReportUART=false;
}
static bool connectPeer(String mac,bool quiet=false){
 if(!enEnabled || !bleReady || workerActive || !validAddr(mac))return false;
 if(subscribed && client && client.isConnected() && currentPeer==mac)return true;
 BLEAdvertisedDevice target;
 for(const BLEAdvertisedDevice& dev:lastScan){
  if(canonical(dev.getAddress().toString())==mac){target=dev;break;}
 }
 if(!target){Serial.printf("BLE peer %s not cached: async discovery\n",mac.c_str());return runScan(false,mac);}
 if(client && client.isConnected())client.disconnect();
 client=BLE.createClient();if(!client)return false;
 workerPeer=mac;workerAddrType=(uint8_t)target.getAddressType();
 workerCancel=false;workerDone=false;workerSuccess=false;
 workerStartedMs=millis();workerActive=true;
 // Copy the advertised address into an immutable heap object to avoid racing
 // updates to scan cache while the worker connects.
 auto* addr=new BTAddress(target.getAddress());
 if(!addr){workerActive=false;return false;}
 // Task parameter ownership transfers to the worker.
 BaseType_t started=xTaskCreate([](void *arg){
   BTAddress address=*(BTAddress*)arg;delete (BTAddress*)arg;
   bool ok=false;
   if(!workerCancel && enEnabled){
    BTStatus st=client.connect(address,7000);
    if(!st)Serial.printf("WORKER_CONNECT_FAILED %s\n",st.toString());
    else if(!workerCancel && enEnabled && client.discoverServices()){
     BLERemoteService svc=client.getService(SERVICE_UUID);
     if(svc){BLERemoteCharacteristic ch=svc.getCharacteristic(TRAINER_UUID);
       if(ch && ch.canNotify() && !workerCancel && enEnabled){
         st=ch.subscribe(BLESubscription::Notify,onNotify);ok=(bool)st;
         if(!ok)Serial.printf("WORKER_SUBSCRIBE_FAILED %s\n",st.toString());
       }
     }
    }
   }
   if(!ok || workerCancel || !enEnabled){ok=false;if(client && client.isConnected())client.disconnect();}
   workerSuccess=ok;
   __atomic_store_n(&workerDone,true,__ATOMIC_RELEASE);
   vTaskDelete(nullptr);
 },"ble-connect",8192,addr,1,nullptr);
 if(started!=pdPASS){delete addr;workerActive=false;Serial.println("WORKER_START_FAILED");return false;}
 Serial.printf("WORKER_START peer=%s type=%d\n",mac.c_str(),workerAddrType);
 return true;
}
static void pollWorker(){
 if(!workerActive)return;
 if(!__atomic_load_n(&workerDone,__ATOMIC_ACQUIRE)){
  if(!enEnabled && !workerCancel){workerCancel=true;client.cancelConnect();}
  if((uint32_t)(millis()-workerStartedMs)>11000 && !workerCancel){
   workerCancel=true;client.cancelConnect();Serial.println("WORKER_DEADLINE_CANCEL");
  }
  return;
 }
 // Worker finished. The BLE client can now be safely managed by main loop.
 bool success=workerSuccess && enEnabled && !workerCancel;
 workerActive=false;workerDone=false;
 if(workerCancel)++connectCancels;
 if(success){
  lastPacketMs=0;
  subscribed=true;currentPeer=workerPeer;connectedAnnounced=true;roleAckRemaining=0;
  if(savedPeer!=workerPeer || savedAddrType!=workerAddrType){
   prefs.putString("peer",workerPeer);prefs.putUChar("addrtype",workerAddrType);
   savedPeer=workerPeer;savedAddrType=workerAddrType;
  }
  linkTuneStage=1;linkTuneAt=millis()+400;
  reply("Connected:"+workerPeer);Serial.printf("WORKER_CONNECTED %s\n",workerPeer.c_str());
 }else if(enEnabled){
  linkTuneStage=0;
  ++connectFailures;
  if(validAddr(savedPeer)){retryAt=millis()+4000;autoReconnectPending=true;}
  Serial.println("WORKER_NOT_CONNECTED");
 }
 if(deferredRadioOff){
  deferredRadioOff=false;
  if(!enEnabled){
   if(bleReady){if(client && client.isConnected())client.disconnect();BLE.end(false);client=BLEClient();lastScan=BLEScan::Results();bleReady=false;++bleStops;}
  }else{
   // A brief EdgeTX reset toggled PH11 back LOW before cancellation ended.
   // BLE stack is still allocated; allow fresh AT handshake to proceed.
   Serial.println("WORKER_RESET_RECOVERY");
  }
 }
}
static void pollLinkTune(){
 if(!enEnabled || !subscribed || workerActive || !client || !client.isConnected()){
   linkTuneStage=0;return;
 }
 if(!linkTuneStage || (int32_t)(millis()-linkTuneAt)<0)return;
 if(linkTuneStage==1){
   const BLEConnParams params={6,6,0,200}; // 7.5ms matched low-latency HT link
   BTStatus st=client.updateConnParams(params);
   Serial.printf("LINK_TUNE_INTERVAL request=7.5ms status=%s\n",st.toString());
   if(st)++linkTuneSuccess;else ++linkTuneErrors;
   linkTuneStage=2;linkTuneAt=millis()+1200;
 }else{
   // 2M PHY is a preference, not required for our legacy GATT payload.
   // The stack may stay on 1M if either peer cannot negotiate 2M.
   BTStatus st=client.setPhy(BLEPhy::PHY_2M,BLEPhy::PHY_2M);
   Serial.printf("LINK_TUNE_PHY request=2M status=%s\n",st.toString());
   if(st)++linkTuneSuccess;else ++linkTuneErrors;
   linkTuneStage=0;
 }
}
static void at(const String& cmd){
 if(!enEnabled)return;
 if(cmd=="AT+OTA"){
   if(anchorOtaActive || anchorOtaPending){
     reply("OK+OTA");return;
   }
   // EdgeTX sends this at the end of ordinary baud/name/power
   // negotiation, before selecting a streaming Trainer/Telemetry role.
   if(initStage<INIT_WAIT_ROLE){reply("ERROR");return;}
   reply("OK+OTA");Serial1.flush();
   anchorOtaPending=true;
   Serial.println("ANCHOR_OTA_REQUEST accepted");
   return;
 }
 if(anchorOtaActive || anchorOtaPending)return;
 Serial.printf("UART> %s\n",cmd.c_str());
 if(cmd=="AT+BAUD4"){
  awaitingHostInit=false;fallbackAllowed=false;wakeRecoveryPending=false;
  initBootFallbackActive=false;
  initAdvance(INIT_WAIT_NAME);
  autoReconnectPending=false;uartSwitch(115200);return;
 }
 if(cmd.startsWith("AT+NAME")){
  String requestedName=cmd.substring(7);
  // The advertising name must fit alongside the service UUID in legacy
  // advertising data. EdgeTX names are normally shorter than this limit.
  if(requestedName.length()>0 && requestedName.length()<=24){
    if(radioBluetoothName!=requestedName){
      radioBluetoothName=requestedName;
      prefs.putString("btName",radioBluetoothName);
    }
  }else{
    Serial.println("BT_NAME ignored empty or overlong AT+NAME payload");
  }
  initAdvance(INIT_WAIT_POWER);
  reply("OK+Name:"+requestedName);replyCentral();return;
 }
 if(cmd.startsWith("AT+TXPW")){
  initAdvance(INIT_WAIT_ROLE);
  reply("OK+Txpw:"+cmd.substring(7));replyCentral();return;
 }
 if(cmd=="AT+ROLE0"){
   // EdgeTX telemetry mirror uses a Bluetooth peripheral role, not trainer-central.
   telemetryRole=true;persistedTelemetryRole=true;
   prefs.putUChar("btRole", 0);
   initAdvance(INIT_COMPLETE);awaitingHostInit=false;
   fallbackAllowed=false;
   autoReconnectPending=false;requestConnect=false;requestScan=false;
   roleAckRemaining=0;
   reply("OK+Role:0");replyPeripheral();
   if(!bleReady){bleStartPending=true;bleStartAt=millis()+250;}
   Serial.println("ROLE0 telemetry mirror peripheral selected");
   return;
 }
 if(cmd=="AT+ROLE1"){
  telemetryRole=false;persistedTelemetryRole=false;
  prefs.putUChar("btRole", 1);
  initAdvance(INIT_COMPLETE);
  initResetPending=false;initRetryBudgetUsed=0;
  reply("OK+Role:1");
  // EdgeTX only advances ROLE_SENT when it reads a Central: line.
  // Schedule it after the transition; repeating once is harmless in IDLE,
  // but must stop immediately if trainer streaming starts.
  roleAckRemaining=2;roleAckAt=millis()+200;
  if(cmd=="AT+ROLE1"){
    awaitingHostInit=false;fallbackAllowed=false;
    if(!bleReady){bleStartPending=true;bleStartAt=millis()+250;}
    if(validAddr(savedPeer)){autoReconnectPending=true;retryAt=millis()+1700;}
  }
  return;
 }
 if(cmd=="AT+DISC?"){requestScan=true;scanRequestReportUART=true;autoReconnectPending=false;return;}
 if(cmd.startsWith("AT+CON")){
  if(telemetryRole){
    // In peripheral mode the phone/computer initiates the connection.
    reply("OK+CONNA");
    if(!telemetryConnected && bleReady)BLE.startAdvertising();
    return;
  }
  String requestedPeer=canonical(cmd.substring(6));
  if(!validAddr(requestedPeer)){reply("ERROR");Serial.println("AT+CON invalid peer address");return;}
  // EdgeTX may request a reconnect after our autonomous recovery already
  // restored the exact peer. Never tear down the established connection.
  if(subscribed && client && client.isConnected() && currentPeer==requestedPeer){
    autoReconnectPending=false;requestConnect=false;
    reply("OK+CONNA");reply("Connected:"+requestedPeer);
    Serial.println("AT+CON already connected; preserved existing BLE session");return;
  }
  selectedPeer=requestedPeer;autoReconnectPending=false;
  reply("OK+CONNA");reply("Connecting to:"+selectedPeer);requestConnect=true;return;
 }
 if(cmd=="AT+CLEAR"){
  autoReconnectPending=false;requestConnect=false;requestScan=false;selectedPeer="";
  clearConnection(false);prefs.remove("peer");prefs.remove("addrtype");
  savedPeer="";savedAddrType=0;reply("OK+CLEAR");return;}
 if(cmd=="AT"){reply("OK");return;}
 reply("ERROR");
}
static void usb(const String& cmd){
 if(cmd=="DROP"){
  if(!enEnabled){Serial.println("RADIO_OFF: DROP ignored");return;}
  bool was=subscribed;clearConnection(was);if(validAddr(savedPeer)){autoReconnectPending=true;retryAt=millis()+2000;}
  Serial.printf("INJECTED_DISCONNECT previouslyConnected=%d retryMs=2000\n",was);return;
 }
 if(cmd=="RESYNC"){
  if(enEnabled){
    requestEdgeTxReset();initLastResetAt=millis();initResetPending=true;++initResyncCount;
    Serial.println("RESYNC sent (manual diagnostics)");
  }
  else Serial.println("RESYNC suppressed: BLE is active or disabled");
  return;
 }
 if(cmd=="SYNCROLE"){if(enEnabled){replyCentral();Serial.println("SYNCROLE sent");}return;}
 if(cmd=="ADDR"){Serial.printf("LOCAL_BT_ADDR %s BLE=%d\n",localBtAddr.c_str(),bleReady);if(bleReady)Serial.printf("BLE_ACTUAL_ADDR %s\n",BLE.getAddress().toString().c_str());return;}
 if(cmd=="BLEFAST"){
   if(!subscribed || !client || !client.isConnected()){Serial.println("BLEFAST disconnected");return;}
   BLEConnParams p={12,12,0,200}; // 15ms interval, 0 latency, 2s supervision
   BTStatus st=client.updateConnParams(p);
   Serial.printf("BLEFAST request interval=15ms latency=0 timeout=2000ms result=%s (asynchronous)\n",st.toString());
   return;
 }
 if(cmd=="BLE7" || cmd=="BLE15"){
   if(!subscribed || !client || !client.isConnected()){Serial.println("BLE interval test: disconnected");return;}
   const uint16_t units=(cmd=="BLE7") ? 6 : 12;
   const BLEConnParams params={units,units,0,200};
   const BTStatus result=client.updateConnParams(params);
   Serial.printf("BLE_INTERVAL_TEST requestedUnits=%u requestedMs=%u.%02u status=%s (asynchronous)\n",
                 (unsigned)units,(unsigned)(units*125/100),(unsigned)(units*125%100),result.toString());
   return;
 }

 if(cmd=="BLE2M"){
   if(!subscribed || !client || !client.isConnected()){Serial.println("BLE2M disconnected");return;}
   BTStatus st=client.setPhy(BLEPhy::PHY_2M,BLEPhy::PHY_2M);
   Serial.printf("BLE2M request result=%s (verify with BLEINFO)\n",st.toString());
   return;
 }
 if(cmd=="BLEINFO"){
   Serial.printf("BLEINFO tuneStage=%u tuneSuccess=%lu tuneErrors=%lu\n",
     (unsigned)linkTuneStage,(unsigned long)linkTuneSuccess,(unsigned long)linkTuneErrors);
   if(!subscribed || !client || !client.isConnected()){Serial.println("BLEINFO disconnected");return;}
   BLEConnInfo info=client.getConnInfo();
   Serial.printf("BLEINFO mtu=%u intervalUnits=%u latency=%u supervisionUnits=%u rssi=%d (Bluedroid interval may be unavailable)\n",
     (unsigned)info.getMTU(),(unsigned)info.getInterval(),(unsigned)info.getLatency(),(unsigned)info.getSupervisionTimeout(),(int)info.getRSSI());
   BLEPhy tx=BLEPhy::PHY_1M,rx=BLEPhy::PHY_1M;
   BTStatus st=client.getPhy(tx,rx);
   Serial.printf("BLEINFO phyStatus=%s txPhy=%u rxPhy=%u\n",st.toString(),(unsigned)tx,(unsigned)rx);
   return;
 }
 if(cmd=="EN"){Serial.printf("PH11 D0/GPIO1=%d transitions=%lu lastChangeMs=%lu enEnabled=%d BLE=%d starts=%lu stops=%lu\n",digitalRead(EN_MONITOR_PIN),(unsigned long)enTransitions,(unsigned long)enChangedAt,enEnabled,bleReady,(unsigned long)bleStarts,(unsigned long)bleStops);return;}
 if(cmd=="PROFILE"){
   Serial.printf("PROFILE rx=%lu superseded=%lu uartPackets=%lu uartBytes=%lu writeMaxUs=%lu mainLoops=%lu mainGapMaxUs=%lu mainGapsOver10ms=%lu\n",
     (unsigned long)notifications,(unsigned long)superseded,(unsigned long)uartPackets,(unsigned long)uartBytes,
     (unsigned long)uartWriteMaxUs,(unsigned long)mainLoops,(unsigned long)mainGapMaxUs,(unsigned long)mainGapsOver10ms);
   Serial.printf("PROFILE_QUEUE maxDepth=%lu maxRxToUartUs=%lu avgRxToUartUs=%lu\n",
     (unsigned long)maxQueueDepth,(unsigned long)maxRxToUartUs,
     (unsigned long)(uartPackets?totalRxToUartUs/uartPackets:0));
   return;
 }
 if(cmd=="OTA"){at("AT+OTA");return;}
 if(cmd=="TELEMSTART"){
   if(!enEnabled){Serial.println("TELEMSTART refused, BT enable GPIO inactive");return;}
   telemetryForcePending=true;autoReconnectPending=false;requestScan=false;requestConnect=false;
   if(workerActive){workerCancel=true;client.cancelConnect();}
   Serial.println("TELEMSTART pending controlled BLE role reinitialization");
   return;
 }
 if(cmd=="TELEMCOMPAT"){
   telemetryViewerCompat=true;telemetryCrsfCount=0;
   Serial.println("TELEMCOMPAT enabled: CRC-validated CRSF 0xC8 -> 0xEA for TelemetryViewer");return;
 }
 if(cmd=="TELEMRAW"){
   telemetryViewerCompat=false;telemetryCrsfCount=0;
   Serial.println("TELEMRAW enabled: no CRSF address translation");return;
 }
 if(cmd=="TELEMTEST"){
   if(!telemetryRole || !telemetryConnected || !telemetryNotify ||
       telemetryNotify.getSubscribedCount()==0){
     Serial.println("TELEMTEST rejected: no subscribed BLE telemetry client");
     return;
   }
   static const char probe[]="C5_TELEM_TEST_v1_ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789";
   for(size_t i=0;i<sizeof(probe)-1;i++)telemetryPush((uint8_t)probe[i]);
   Serial.printf("TELEMTEST queued=%u\n",(unsigned)(sizeof(probe)-1));
   return;
 }
 if(cmd=="TELEMSTATUS"){
  Serial.printf("TELEM role=%d connected=%d advertising=%d BLE=%d subscribed=%u uartBytes=%lu bleBytes=%lu blePackets=%lu dropped=%lu busy=%lu queued=%u maxDepth=%lu peer=%s\n",
    telemetryRole,telemetryConnected,(int)(bleReady && BLE.getAdvertising().isAdvertising()),bleReady,(unsigned)(telemetryNotify?telemetryNotify.getSubscribedCount():0),
    (unsigned long)telemetryUartBytes,(unsigned long)telemetryBleBytes,(unsigned long)telemetryBlePackets,
    (unsigned long)telemetryDrops,(unsigned long)telemetryNotifyBusy,
    (unsigned)telemetryDepth(),(unsigned long)telemetryMaxDepth,telemetryPeer.c_str());
  Serial.printf("TELEM noSubscriberDropped=%lu sendFailures=%lu savedRole=%s\n",
     (unsigned long)telemetryNoSubscriberDrops,(unsigned long)telemetrySendFailures,
     persistedTelemetryRole?"telemetry":"trainer");
  Serial.printf("TELEM CRSF-compat=%d converted=%lu invalid=%lu pending=%u\n",
    telemetryViewerCompat,(unsigned long)telemetryCrsfConverted,
    (unsigned long)telemetryCrsfInvalid,(unsigned)telemetryCrsfCount);return;
 }
 if(cmd=="STATUS"){
  Serial.printf("WORKER active=%d done=%d cancel=%d connected=%d failures=%lu cancels=%lu age=%lu\n",workerActive,workerDone,workerCancel,subscribed,(unsigned long)connectFailures,(unsigned long)connectCancels,(unsigned long)(workerActive?millis()-workerStartedMs:0));
  Serial.printf("LINK maxGapMs=%lu gapsOver100ms=%lu malformed=%lu stalls=%lu\n",(unsigned long)maxGapMs,(unsigned long)gapsOver100Ms,(unsigned long)malformedNotifications,(unsigned long)linkStalls);
  Serial.printf("POWER wakeFromDeep=%d recoveryPending=%d recoveryAttempts=%lu offSince=%lu sleepHighMs=%lu\n",deepSleepWake,wakeRecoveryPending,(unsigned long)recoveryAttempts,(unsigned long)offSince,(unsigned long)DEEP_SLEEP_HIGH_MS);
  Serial.printf("INIT stage=%u ageMs=%lu complete=%d bootFallback=%d resyncs=%lu retryUsed=%u/%u pending=%d lastResetMs=%lu\n",
   (unsigned)initStage,(unsigned long)(initStageAt?millis()-initStageAt:0),initStage==INIT_COMPLETE,
   initBootFallbackActive,(unsigned long)initResyncCount,(unsigned)initRetryBudgetUsed,(unsigned)INIT_MAX_RETRIES,
   initResetPending,(unsigned long)initLastResetAt);
  Serial.printf("SCAN_STATE active=%d report=%d starts=%lu completes=%lu cancels=%lu pendingPeer=%s\n",scanActive,scanReportUART,(unsigned long)scanStarts,(unsigned long)scanCompletes,(unsigned long)scanCancels,scanRequestedPeer.c_str());
  Serial.printf("STATE BLE=%d UART=%lu subscribed=%d realPeer=%s savedPeer=%s type=%u notifications=%lu dropped=%lu superseded=%lu lastAge=%lu\n",bleReady,(unsigned long)uartBaud,subscribed,currentPeer.c_str(),savedPeer.c_str(),savedAddrType,(unsigned long)notifications,(unsigned long)dropped,(unsigned long)superseded,(unsigned long)(lastPacketMs?millis()-lastPacketMs:0));return;
 }
 if(cmd=="SCAN"){if(enEnabled){requestScan=true;scanRequestReportUART=false;}else Serial.println("RADIO_OFF: SCAN blocked");return;}
 if(cmd.startsWith("CONNECT ")){if(!enEnabled){Serial.println("RADIO_OFF: CONNECT blocked");return;}selectedPeer=canonical(cmd.substring(8));if(!validAddr(selectedPeer)){Serial.println("Bad peer address");return;}requestConnect=true;return;}
 if(cmd=="FORGET"){
  prefs.remove("peer");prefs.remove("addrtype");savedPeer="";savedAddrType=0;
  selectedPeer="";requestConnect=false;autoReconnectPending=false;
  Serial.println("NVS peer removed");return;}
 if(cmd=="RESETUART"){uartSwitch(57600);return;}
 if(cmd=="UART115200"){if(!enEnabled){Serial.println("RADIO_OFF: UART115200 blocked");return;}awaitingHostInit=false;uartSwitch(115200);return;}
 if(cmd=="REBOOT"){Serial.println("C5 controlled reboot");Serial.flush();ESP.restart();return;}
 Serial.println("Commands: BLEINFO BLEFAST BLE7 BLE15 BLE2M STATUS EN ADDR RESYNC SYNCROLE SCAN CONNECT <12hex> DROP FORGET RESETUART UART115200 REBOOT");
}
void setup(){
 Serial.begin(115200);readLocalBtAddr();pinMode(EN_MONITOR_PIN,INPUT);enLast=digitalRead(EN_MONITOR_PIN);enCandidate=enLast;enCandidateAt=millis();enChangedAt=millis();enEnabled=(enLast==LOW);Serial1.begin(57600,SERIAL_8N1,RX_PIN,TX_PIN);
 packetQueue=xQueueCreate(4,sizeof(Packet));
 prefs.begin("c5trainer",false);savedPeer=canonical(prefs.getString("peer",""));savedAddrType=prefs.getUChar("addrtype",0);
 persistedTelemetryRole=(prefs.getUChar("btRole",1)==0);
 String storedName=prefs.getString("btName","");
 if(storedName.length()>0 && storedName.length()<=24)radioBluetoothName=storedName;
 bootAt=millis();
 // C5-only reboot while EdgeTX stays enabled is the one case where its
 // factory AT sequence will not repeat. Allow the old fallback at boot only.
 if(enEnabled){
  radioOn();
  // After deep-sleep wake, EdgeTX may have sent factory-baud AT+BAUD4
  // before our UART became ready. Do not auto-connect or stream to EdgeTX
  // until the handshake is explicitly recovered.
  deepSleepWake=esp_sleep_get_wakeup_cause()==ESP_SLEEP_WAKEUP_EXT1;
  if(deepSleepWake){wakeRecoveryPending=true;wakeRecoveryAt=millis()+1100;fallbackAllowed=false;}
  else fallbackAllowed=true;
 }else Serial.println("RADIO_OFF at boot: BLE not initialized");
 if(!enEnabled)offSince=millis();
 Serial.printf("C5_SLEEP_TEST WAKE_CAUSES=0x%08lX\n",(unsigned long)esp_sleep_get_wakeup_causes());
 Serial.printf("ANCHOR_C5 BLE=%d savedPeer=%s factoryBaud=%lu\n",bleReady,savedPeer.c_str(),(unsigned long)uartBaud);
}
void loop(){
 if(anchorOtaActive || anchorOtaPending){
   anchorOtaPoll();
   delay(1);
   return;
 }
 const uint32_t nowUs=micros();
 if(mainLastUs){uint32_t gap=nowUs-mainLastUs;
   if(gap>mainGapMaxUs)mainGapMaxUs=gap;
   if(gap>10000)++mainGapsOver10ms;
 }
 mainLastUs=nowUs;++mainLoops;
 readEnable();
 // Only 2500 continuous milliseconds HIGH permits deep sleep. Brief EdgeTX
 // disable/re-init pulses are serviced awake with UART already at 57600.
 // Abort unless LOW-level GPIO1 wake can be configured successfully.
 if(!enEnabled && !workerActive && !deferredRadioOff && offSince && !sleepAttempted && (uint32_t)(millis()-offSince)>=DEEP_SLEEP_HIGH_MS && digitalRead(EN_MONITOR_PIN)==HIGH){
  sleepAttempted=true;
  esp_err_t rc=esp_sleep_enable_ext1_wakeup_io(1ULL<<EN_MONITOR_PIN, ESP_EXT1_WAKEUP_ANY_LOW);
  Serial.printf("SLEEP_ARM GPIO1 LOW rc=%d (%s)\n",(int)rc,esp_err_to_name(rc));
  Serial.flush();
  if(rc==ESP_OK && digitalRead(EN_MONITOR_PIN)==HIGH){
    Serial.println("ENTER_DEEP_SLEEP");Serial.flush();delay(100);
    if(digitalRead(EN_MONITOR_PIN)==HIGH)esp_deep_sleep_start();
  }
 }

 while(enEnabled && Serial1.available()){
  uint8_t byte=(uint8_t)Serial1.read();
  if(telemetryRole && initStage==INIT_COMPLETE){
    // EdgeTX sends binary CRSF in telemetry role *even when there is no
    // BLE client*. Never parse that stream as AT on BLE disconnect; doing
    // so can emit ERROR to EdgeTX and induce a destructive reset loop.
    if(telemetryConnected)telemetryInputByte(byte);
    else telemetryCrsfCount=0;
  }else{
    telemetryCrsfCount=0;
    char ch=(char)byte;
    if(ch=='\r'||ch=='\n'){if(uartLine.length()){at(uartLine);uartLine="";}}
    else if(uartLine.length()<79)uartLine+=ch;else uartLine="";
  }
 }
 while(Serial.available()){
  char c=(char)Serial.read();if(c=='\r'||c=='\n'){if(usbLine.length()){usb(usbLine);usbLine="";}}
  else if(usbLine.length()<79)usbLine+=c;else usbLine="";
 }
 // If the C5 rebooted while EdgeTX remained running, EdgeTX is at 115200
 // and will not send its factory-baud AT initialization again. Give a startup
 // window for AT+BAUD4, then restore the saved BLE peer autonomously.
 // Deep-sleep wake can miss EdgeTX's one-shot 57600-baud startup request.
 // If no real handshake was seen, transmit ERROR at operational baud,
 // which EdgeTX's readline(error_reset=true) treats as a reset request.
 // Do NOT reconnect BLE before a clean EdgeTX AT+BAUD4/ROLE sequence.
 if(enEnabled && wakeRecoveryPending && awaitingHostInit &&
    initRetryBudgetUsed<INIT_MAX_RETRIES &&
    (int32_t)(millis()-wakeRecoveryAt)>=0){
   wakeRecoveryPending=false;
   uartSwitch(115200);
   requestEdgeTxReset();
   initLastResetAt=millis();initResetPending=true;++initResyncCount;++initRetryBudgetUsed;
   ++recoveryAttempts;
   Serial.printf("WAKE_RECOVERY_REQUEST attempt=%lu; awaiting PH11 reset\n",(unsigned long)recoveryAttempts);
 }
 // A C5-only reboot is distinct from an EdgeTX PH11 reset. If EdgeTX
 // remains in its CONNECTED trainer state, it will not send AT commands,
 // and receiveTrainer() ignores ERROR. Restore the saved BLE peer only on
 // that boot-only path, never after a PH11 HIGH->LOW transition.
 if(enEnabled && fallbackAllowed && awaitingHostInit &&
    initStage==INIT_WAIT_BAUD && (uint32_t)(millis()-bootAt)>=3000){
   fallbackAllowed=false;awaitingHostInit=false;
   initBootFallbackActive=true;
   uartSwitch(115200);
   if(persistedTelemetryRole){
     // EdgeTX may remain in TELEMETRY mode and never repeat AT+ROLE0.
     // Restore the actual BLE peripheral instead of the old trainer central.
     telemetryRole=true;
     initAdvance(INIT_COMPLETE);
     bleStartPending=true;bleStartAt=millis()+50;
     autoReconnectPending=false;requestScan=false;requestConnect=false;
     Serial.println("BOOT_ONLY_FALLBACK saved role0: restore BLE telemetry advertising");
   }else{
     if(validAddr(savedPeer)){
       bleStartPending=true;bleStartAt=millis()+50;
       autoReconnectPending=true;retryAt=millis()+1200;
     }
     Serial.println("BOOT_ONLY_FALLBACK saved role1: restore trainer central");
   }
 }
 // Init watchdog is only active when PH11 is LOW and a UART initialization
 // stage has not completed. It never tears down a working BLE session.
 // Delayed reset requests are rate-limited, and a real PH11 HIGH->LOW
 // transition resets the stage deadlines. EdgeTX may briefly remain in
 // its old state while it resets; we do not spray repeated ERROR lines.
 if(enEnabled && !initBootFallbackActive && initStage!=INIT_COMPLETE && initStageAt &&
    initRetryBudgetUsed<INIT_MAX_RETRIES &&
    (uint32_t)(millis()-initStageAt) >=
       (initStage==INIT_WAIT_BAUD ? INIT_FIRST_BAUD_TIMEOUT_MS : INIT_STAGE_TIMEOUT_MS) &&
    (!initLastResetAt || (uint32_t)(millis()-initLastResetAt)>=INIT_RECOVERY_COOLDOWN_MS)){
   // EdgeTX is at 115200 after AT+BAUD4. For a missed AT+BAUD4,
   // EdgeTX also eventually moves to operational baud. Force a reset
   // request there; the radio's own recovery uses PH11 HIGH for ~1s.
   if(uartBaud!=115200)uartSwitch(115200);
   requestEdgeTxReset();
   initLastResetAt=millis();initResetPending=true;++initResyncCount;++initRetryBudgetUsed;
   Serial.printf("INIT_WATCHDOG_RESET stage=%u elapsedMs=%lu attempt=%lu\n",
     (unsigned)initStage,(unsigned long)(millis()-initStageAt),
     (unsigned long)initResyncCount);
 }
 if(enEnabled && roleAckRemaining && !subscribed && (int32_t)(millis()-roleAckAt)>=0){
  replyCentral();--roleAckRemaining;roleAckAt=millis()+450;
 }
 if(enEnabled && bleStartPending && (int32_t)(millis()-bleStartAt)>=0)startBLE();
 if(telemetryRole){
   // BLE callbacks only set flags. UART protocol replies and advertising
   // operations happen on the ordinary main task, not in BLE callbacks.
   if(telemetryNotifyConnect){
     telemetryNotifyConnect=false;
     // An ESP boot-ROM byte can remain in EdgeTX readline() after a
     // C5-only restart. Terminate that partial record before announcing
     // the connection, otherwise EdgeTX remains IDLE despite an active BLE
     // session. A blank CRLF is ignored by EdgeTX.
     Serial1.print("\r\n");
     reply("Connected:"+telemetryPeer);
     Serial.printf("TELEM_CLIENT_CONNECTED %s\n",telemetryPeer.c_str());
   }
   if(telemetryNotifyDisconnect){
     telemetryNotifyDisconnect=false;reply("DisConnected");
     Serial.println("TELEM_CLIENT_DISCONNECTED");
   }
   if(telemetryStartAdvertising && enEnabled && bleReady){
     telemetryStartAdvertising=false;BLE.startAdvertising();
   }
   telemetryDrain();
 }else{
   pollWorker();
   pollLinkTune();
 }
 if(telemetryForcePending && !workerActive){
   telemetryForcePending=false;
   autoReconnectPending=false;requestScan=false;requestConnect=false;
   if(scanActive && bleReady){BLE.getScan().stop();scanActive=false;}
   clearConnection(false);
   if(bleReady){
     BLE.end(false);bleReady=false;++bleStops;
     client=BLEClient();lastScan=BLEScan::Results();
   }
   telemetryServer=BLEServer();telemetryNotify=BLECharacteristic();telemetryWrite=BLECharacteristic();
   telemetryConnected=false;telemetryStartAdvertising=false;telemetryHead=telemetryTail=0;
   at("AT+ROLE0");
   Serial.println("TELEMSTART ROLE0 command injected through C5 control (not UART)");
 }
 // Diagnostic-only until packet-gap behavior has been characterized.
 // Do NOT drop a healthy BLE link merely because a notification gap occurs.
 // 1.5s automatic teardown caused repeated false positives on real hardware.
 // Snapshot NOW first, then the callback-maintained timestamp. If the
 // callback records a newer timestamp in between, signed age is negative,
 // NOT a wrapped 0xFFFFFFFF value. Never trigger on that ordering race.
 const uint32_t watchdogNow=millis();
 const uint32_t watchdogLast=__atomic_load_n(&lastPacketMs,__ATOMIC_ACQUIRE);
 const int32_t watchdogAge=(int32_t)(watchdogNow-watchdogLast);
 if(enEnabled && subscribed && watchdogLast && watchdogAge>=5000 &&
    (int32_t)(watchdogNow-nextStallCheck)>=0){
  ++linkStalls;nextStallCheck=watchdogNow+5000;
  Serial.printf("LINK_STALL_DIAGNOSTIC ageMs=%ld connected=%d (no forced disconnect)\n",
      (long)watchdogAge,client && client.isConnected());
 }
 if(enEnabled && !telemetryRole && bleReady && !workerActive && requestScan){requestScan=false;runScan(scanRequestReportUART);scanRequestReportUART=false;}
 if(enEnabled && !telemetryRole && bleReady && !workerActive)pollScan();
 if(enEnabled && !telemetryRole && bleReady && !workerActive && requestConnect){requestConnect=false;if(!connectPeer(selectedPeer)){reply("ERROR");retryAt=millis()+2000;}}
 if(enEnabled && !telemetryRole && bleReady && !workerActive && autoReconnectPending && (int32_t)(millis()-retryAt)>=0 && validAddr(savedPeer)){
  autoReconnectPending=false;
  if(!subscribed && !connectPeer(savedPeer,true)){retryAt=millis()+4000;autoReconnectPending=true;}
 }
 if(enEnabled && !telemetryRole && !workerActive && subscribed && client && !client.isConnected()){
  clearConnection(true);if(validAddr(savedPeer)){autoReconnectPending=true;retryAt=millis()+2500;}
 }
 Packet packet;
 while(packetQueue && xQueueReceive(packetQueue,&packet,0)==pdTRUE){
  if(enEnabled && subscribed && (initStage==INIT_COMPLETE || initBootFallbackActive))
    { const uint32_t t0=micros();
      size_t written=Serial1.write(packet.data,packet.len);
      uint32_t us=micros()-t0;
      if(us>uartWriteMaxUs)uartWriteMaxUs=us;
      if(written==packet.len)++uartPackets;
      uartBytes+=(uint32_t)written;
      uint32_t age=micros()-packet.receivedUs;
      if(age>maxRxToUartUs)maxRxToUartUs=age;
      totalRxToUartUs+=age;
    }
 }
 if(millis()-lastLog>15000){lastLog=millis();Serial.printf("HEARTBEAT connected=%d notifications=%lu dropped=%lu\n",subscribed,(unsigned long)notifications,(unsigned long)dropped);}
}
