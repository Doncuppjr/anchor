#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/printk.h>
#include <zephyr/init.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <hal/nrf_power.h>
#include <string.h>
#include <errno.h>
#include "trainer.h"
static uint32_t startup_gp, startup_gp2;
static int capture_boot_regs(void){startup_gp=NRF_POWER->GPREGRET;startup_gp2=NRF_POWER->GPREGRET2;return 0;}
SYS_INIT(capture_boot_regs, PRE_KERNEL_1, 0);
static const struct device *console=DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static const struct device *gpio0=DEVICE_DT_GET(DT_NODELABEL(gpio0));
static int bt_enable_level=-1;
static int vcc_enable_init(void){if(!device_is_ready(gpio0))return -ENODEV;return gpio_pin_configure(gpio0,13,GPIO_OUTPUT_INACTIVE);}
static int led_init(void){if(!device_is_ready(gpio0))return -ENODEV;return gpio_pin_configure(gpio0,15,GPIO_OUTPUT_INACTIVE);}
static int vcc_set(int level){return gpio_pin_set(gpio0,13,level);}
static int vcc_level(void){return gpio_pin_get(gpio0,13);}
static int led_set(int level){return gpio_pin_set(gpio0,15,level);}
static int bt_enable_init(void){if(!device_is_ready(gpio0))return -ENODEV;return gpio_pin_configure(gpio0,2,GPIO_INPUT);}
static int bt_enable_read(void){return gpio_pin_get(gpio0,2);}
RING_BUF_DECLARE(rx,256);
static struct bt_conn *peer;
static struct bt_gatt_discover_params disc;
static struct bt_gatt_subscribe_params sub;
static struct bt_uuid_16 service=BT_UUID_INIT_16(0xfff0);
static struct bt_uuid_16 trainer=BT_UUID_INIT_16(0xfff6);
static atomic_t packets, bytes, valid, invalid, boot;
static struct k_spinlock sample_lock;
static uint16_t latest[8];
static uint8_t latest_wire[30];
static size_t latest_len;
static const struct device *wire_uart=DEVICE_DT_GET(DT_NODELABEL(uart0));
static uint8_t dma_frame[30];
static uint32_t generation;
static atomic_t uart_enabled, uart_done, uart_abort, uart_error, uart_skipped;
static atomic_t at_baud;
static uint8_t uart_start_retries;
static bool update_authorized;
static uint8_t uart_rx_buf[128], uart_rx_buf_pool[3][128];
static bool rx_buf_busy[4];
K_MSGQ_DEFINE(at_q,sizeof(uint8_t),256,4);
static struct k_sem at_ready;
static char at_line[64];
static size_t at_line_len;
static K_SEM_DEFINE(frame_ready,0,1);
static K_SEM_DEFINE(tx_finished,0,1);
K_MUTEX_DEFINE(uart_tx_mutex);
static void wire_event(const struct device *dev,struct uart_event *evt,void *ctx){
 if(evt->type==UART_RX_BUF_REQUEST){
  int idx=-1;for(int i=0;i<4;i++)if(!rx_buf_busy[i]){idx=i;break;}
  if(idx>=0){uint8_t *b=idx==0?uart_rx_buf:uart_rx_buf_pool[idx-1];rx_buf_busy[idx]=true;int e=uart_rx_buf_rsp(dev,b,128);if(e)rx_buf_busy[idx]=false;printk("UART RX_BUF_REQUEST idx=%d rsp=%d\n",idx,e);}
  else printk("UART RX_BUF_REQUEST NO FREE BUFFER\n");
 }else if(evt->type==UART_RX_RDY){
  for(size_t i=0;i<evt->data.rx.len;i++){uint8_t c=evt->data.rx.buf[evt->data.rx.offset+i];if(k_msgq_put(&at_q,&c,K_NO_WAIT))printk("AT RX queue full\n");}
  printk("AT RX bytes=%u\n",evt->data.rx.len);
 }else if(evt->type==UART_RX_BUF_RELEASED){
  for(int i=0;i<4;i++){uint8_t *b=i==0?uart_rx_buf:uart_rx_buf_pool[i-1];if(b==evt->data.rx_buf.buf)rx_buf_busy[i]=false;}
 }else if(evt->type==UART_RX_STOPPED){
  printk("UART RX_STOPPED reason=%u\n",evt->data.rx_stop.reason);
 }else if(evt->type==UART_RX_DISABLED){
  printk("UART RX_DISABLED\n");
  if(atomic_get(&uart_enabled)){memset(rx_buf_busy,0,sizeof(rx_buf_busy));rx_buf_busy[0]=true;int e=uart_rx_enable(dev,uart_rx_buf,sizeof(uart_rx_buf),10000);printk("UART RX_RESTART=%d\n",e);}
 }else if(evt->type==UART_TX_DONE){atomic_inc(&uart_done);k_sem_give(&tx_finished);}
 else if(evt->type==UART_TX_ABORTED){atomic_inc(&uart_abort);k_sem_give(&tx_finished);}
}
static void wire_thread(void *a,void *b,void *c){
 uint32_t consumed=0;
 while(1){
  k_sem_take(&frame_ready,K_FOREVER);
  if(!atomic_get(&uart_enabled))continue;
  k_spinlock_key_t key=k_spin_lock(&sample_lock);
  size_t n=latest_len;uint32_t seq=generation;
  if(n&&seq!=consumed)memcpy(dma_frame,latest_wire,n);
  k_spin_unlock(&sample_lock,key);
  if(!n||seq==consumed)continue;
  if(consumed&&seq-consumed>1)atomic_add(&uart_skipped,seq-consumed-1);
  consumed=seq;
  k_mutex_lock(&uart_tx_mutex,K_FOREVER);k_sem_reset(&tx_finished);
  int e=uart_tx(wire_uart,dma_frame,n,SYS_FOREVER_US);
  if(e){atomic_inc(&uart_error);k_mutex_unlock(&uart_tx_mutex);continue;}
  k_sem_take(&tx_finished,K_FOREVER);k_mutex_unlock(&uart_tx_mutex);
 }
}
K_THREAD_DEFINE(wire_tid,1024,wire_thread,NULL,NULL,NULL,5,0,0);
static int wire_start(void){
 if(atomic_get(&uart_enabled))return 0;
 if(!device_is_ready(wire_uart)){printk("UART not ready\n");return -ENODEV;}
 struct uart_config cfg={.baudrate=57600,.parity=UART_CFG_PARITY_NONE,.stop_bits=UART_CFG_STOP_BITS_1,.data_bits=UART_CFG_DATA_BITS_8,.flow_ctrl=UART_CFG_FLOW_CTRL_NONE};
 int e=uart_configure(wire_uart,&cfg);
 if(!e)e=uart_callback_set(wire_uart,wire_event,NULL);
 if(!e){
  memset(rx_buf_busy,0,sizeof(rx_buf_busy));
  rx_buf_busy[0]=true;
  e=uart_rx_enable(wire_uart,uart_rx_buf,sizeof(uart_rx_buf),10000);
 }
 if(!e){
  atomic_set(&at_baud,57600);
  atomic_set(&uart_enabled,1);
 }else{
  memset(rx_buf_busy,0,sizeof(rx_buf_busy));
 }
 printk("UART_START result=%d baud=57600 enabled=%d\n",e,(int)atomic_get(&uart_enabled));
 return e;
}
static uint16_t end_handle;
static void scan(void);
static void scan_work_fn(struct k_work *w){scan();}
static K_WORK_DELAYABLE_DEFINE(scan_work,scan_work_fn);
static bool start_requested, started, role_active;
static bt_addr_le_t anchor_identity;
static void serial_irq(const struct device *dev, void *ctx){
 uint8_t b[32]; if(!uart_irq_update(dev))return;
 while(uart_irq_rx_ready(dev)){int n=uart_fifo_read(dev,b,sizeof(b));if(n<=0)break;ring_buf_put(&rx,b,n);}
}
static void uart_reply(const char *s){
 if(!atomic_get(&uart_enabled))return;
 static uint8_t tx[96];size_t n=strlen(s);if(n>sizeof(tx)-2)return;
 memcpy(tx,s,n);tx[n++]='\r';tx[n++]='\n';
 k_mutex_lock(&uart_tx_mutex,K_FOREVER);k_sem_reset(&tx_finished);
 int e=uart_tx(wire_uart,tx,n,SYS_FOREVER_US);
 if(!e)k_sem_take(&tx_finished,K_FOREVER);
 k_mutex_unlock(&uart_tx_mutex);
}
static void at_identity(const char *prefix){
 char mac[BT_ADDR_LE_STR_LEN];bt_addr_le_to_str(&anchor_identity,mac,sizeof(mac));char *suffix=strstr(mac," (");if(suffix)*suffix=0;char line[64];snprintf(line,sizeof(line),"%s%s",prefix,mac);uart_reply(line);
}
static void at_command(const char *s){
 printk("UART AT< %s\n",s);
 if(!strcmp(s,"AT"))uart_reply("OK");
 else if(!strcmp(s,"TESTBAUD57600")){struct uart_config c={.baudrate=57600,.parity=UART_CFG_PARITY_NONE,.stop_bits=UART_CFG_STOP_BITS_1,.data_bits=UART_CFG_DATA_BITS_8,.flow_ctrl=UART_CFG_FLOW_CTRL_NONE};int e=uart_configure(wire_uart,&c);atomic_set(&at_baud,e?115200:57600);printk("UART test baud=%ld result=%d\n",(long)atomic_get(&at_baud),e);}
 else if(!strcmp(s,"AT+BAUD4")){
  uart_reply("OK");k_sleep(K_MSEC(50));
  struct uart_config c={.baudrate=115200,.parity=UART_CFG_PARITY_NONE,.stop_bits=UART_CFG_STOP_BITS_1,.data_bits=UART_CFG_DATA_BITS_8,.flow_ctrl=UART_CFG_FLOW_CTRL_NONE};
  int e=uart_configure(wire_uart,&c);atomic_set(&at_baud,e?57600:115200);
  printk("UART BAUD4 result=%d baud=%ld\n",e,(long)atomic_get(&at_baud));
 }else if(!strncmp(s,"AT+NAME",7)){char line[64];snprintf(line,sizeof(line),"OK+Name:%.55s",s+7);uart_reply(line);}
 else if(!strncmp(s,"AT+TXPW",7)){char line[32];snprintf(line,sizeof(line),"OK+Txpw:%.23s",s+7);uart_reply(line);uart_reply("OK+CAPS:1,UPDATE_UART");at_identity("Central:");}
 else if(!strcmp(s,"AT+UPDATE=UART")){if(role_active){uart_reply("ERROR");return;}update_authorized=true;uart_reply("OK+UPDATE:UART");}
 else if(!strcmp(s,"AT+DOWNLOAD")){if(!update_authorized){uart_reply("ERROR");return;}uart_reply("OK+DOWNLOAD");k_sleep(K_MSEC(100));NRF_POWER->GPREGRET=0x59;sys_reboot(SYS_REBOOT_COLD);}
 else if(!strcmp(s,"AT+ROLE1")){uart_reply("OK+Role:1");at_identity("Central:");role_active=true;if(!started){int e=bt_enable(NULL);printk("AT BLE init=%d\n",e);if(!e)started=true;}if(started)scan();}
 else if(!strcmp(s,"AT+ROLE0")){uart_reply("OK+Role:0");at_identity("Peripheral:");role_active=true;if(!started){int e=bt_enable(NULL);printk("AT BLE init=%d\n",e);if(!e)started=true;}}
 else if(!strcmp(s,"AT+DISC?")){uart_reply("OK+DISCS");}
 else if(!strncmp(s,"AT+CON",6)){uart_reply("OK+CONNA");}
 else uart_reply("ERROR");
}
static void uart_commands(void){
 uint8_t c;
 while(k_msgq_get(&at_q,&c,K_NO_WAIT)==0){
  if(c=='\r'||c=='\n'){
   if(at_line_len){at_line[at_line_len]=0;at_command(at_line);at_line_len=0;}
  }else if(c<0x20||c>0x7e){printk("AT RX discarded nonprintable=%02x\n",c);at_line_len=0;}
  else if(at_line_len<sizeof(at_line)-1)at_line[at_line_len++]=(char)c;
  else at_line_len=0;
 }
}
static void commands(void){
 static char b[48];static unsigned n;uint8_t c;
 while(ring_buf_get(&rx,&c,1)){
 if(c==10||c==13){if(n){b[n]=0;if(!strcmp(b,"UART_DFU")||!strcmp(b,"AT+DOWNLOAD")){printk("UART DFU reboot\n");k_sleep(K_MSEC(100));NRF_POWER->GPREGRET=0x59;sys_reboot(SYS_REBOOT_COLD);}else if(!strcmp(b,"UF2")){printk("UF2 reboot\n");k_sleep(K_MSEC(100));NRF_POWER->GPREGRET=0x57;sys_reboot(SYS_REBOOT_COLD);}else if(!strcmp(b,"TESTBAUD57600")){if(atomic_get(&uart_enabled)){struct uart_config c={.baudrate=57600,.parity=UART_CFG_PARITY_NONE,.stop_bits=UART_CFG_STOP_BITS_1,.data_bits=UART_CFG_DATA_BITS_8,.flow_ctrl=UART_CFG_FLOW_CTRL_NONE};int e=uart_configure(wire_uart,&c);atomic_set(&at_baud,e?115200:57600);printk("UART test baud=%ld result=%d\n",(long)atomic_get(&at_baud),e);}}else if(!strcmp(b,"UART_START")){uart_start_retries=5;wire_start();}
 else if(!strcmp(b,"VCC_LOW")){int e=vcc_set(0);printk("VCC P0.13=LOW result=%d readback=%d\\n",e,vcc_level());}
 else if(!strcmp(b,"VCC_HIGH")){int e=vcc_set(1);printk("VCC P0.13=HIGH result=%d readback=%d\\n",e,vcc_level());}else if(!strcmp(b,"BT_START")){role_active=true;start_requested=true;}else if(!strcmp(b,"PING")){int en=bt_enable_read();int vcc=gpio_pin_get(gpio0,13);printk("PONG ANCHOR GPREGRET=%02x GPREGRET2=%02x START=%02x/%02x MARK=%08x BT_EN_P0.02=%s(%d) VCC_EN_P0.13=%s(%d)\n",(unsigned)NRF_POWER->GPREGRET,(unsigned)NRF_POWER->GPREGRET2,(unsigned)startup_gp,(unsigned)startup_gp2,(unsigned)(*((volatile uint32_t*)0x20007F78)),en>0?"HIGH":en==0?"LOW":"ERROR",en,vcc>0?"HIGH":vcc==0?"LOW":"ERROR",vcc);}n=0;}}
 else if(n<sizeof(b)-1)b[n++]=c;else n=0;
 }}
static uint8_t notify(struct bt_conn *c,struct bt_gatt_subscribe_params *p,const void *data,uint16_t len){
 if(!data)return BT_GATT_ITER_STOP;
 atomic_inc(&packets);atomic_add(&bytes,len);
 const uint8_t *b=data;uint16_t ch[8];
 if(len==2&&b[0]==13&&b[1]==10)atomic_inc(&boot);
 else if(trainer_decode(b,len,ch)){
  k_spinlock_key_t key=k_spin_lock(&sample_lock);
  memcpy(latest,ch,sizeof(latest));memcpy(latest_wire,b,len);latest_len=len;generation++;
  k_spin_unlock(&sample_lock,key);atomic_inc(&valid);k_sem_give(&frame_ready);
 }else atomic_inc(&invalid);
 return BT_GATT_ITER_CONTINUE;
}
static uint8_t discover(struct bt_conn *c,const struct bt_gatt_attr *a,struct bt_gatt_discover_params *p){
 if(!a){printk("Discovery failed stage=%u\n",p->type);bt_conn_disconnect(c,BT_HCI_ERR_REMOTE_USER_TERM_CONN);return BT_GATT_ITER_STOP;}
 int err;
 if(p->type==BT_GATT_DISCOVER_PRIMARY){
 struct bt_gatt_service_val *v=a->user_data;char u[40];bt_uuid_to_str(v->uuid,u,sizeof(u));printk("SERVICE %s handles=%u-%u\n",u,a->handle,v->end_handle);if(bt_uuid_cmp(v->uuid,&service.uuid))return BT_GATT_ITER_CONTINUE;end_handle=v->end_handle;
 p->uuid=NULL;p->start_handle=a->handle+1;p->end_handle=end_handle;p->type=BT_GATT_DISCOVER_CHARACTERISTIC;
 err=bt_gatt_discover(c,p);
 }else if(p->type==BT_GATT_DISCOVER_CHARACTERISTIC){
 struct bt_gatt_chrc *v=a->user_data;char u[40];bt_uuid_to_str(v->uuid,u,sizeof(u));printk("CHAR %s handle=%u value=%u props=%u\n",u,a->handle,v->value_handle,v->properties);if(bt_uuid_cmp(v->uuid,&trainer.uuid))return BT_GATT_ITER_CONTINUE;sub.value_handle=bt_gatt_attr_value_handle(a);p->uuid=BT_UUID_GATT_CCC;p->start_handle=sub.value_handle+1;p->type=BT_GATT_DISCOVER_DESCRIPTOR;
 err=bt_gatt_discover(c,p);
 }else{
 sub.ccc_handle=a->handle;sub.notify=notify;sub.value=BT_GATT_CCC_NOTIFY;
 err=bt_gatt_subscribe(c,&sub);printk("Subscribe result=%d value=%u ccc=%u\n",err,sub.value_handle,sub.ccc_handle);
 }
 if(err){printk("GATT error=%d\n",err);bt_conn_disconnect(c,BT_HCI_ERR_REMOTE_USER_TERM_CONN);}
 return BT_GATT_ITER_STOP;
}
static void connected(struct bt_conn *c,uint8_t err){
 if(err){printk("Connection failed=%u\n",err);bt_conn_unref(peer);peer=NULL;k_work_reschedule(&scan_work,K_SECONDS(1));return;}
 printk("Connected BLE\n");
 struct bt_conn_info i;if(!bt_conn_get_info(c,&i))printk("interval_us=%u latency=%u\n",i.le.interval_us,i.le.latency);
 
 memset(&disc,0,sizeof(disc));memset(&sub,0,sizeof(sub));
 disc.uuid=NULL;disc.func=discover;disc.start_handle=1;disc.end_handle=0xffff;disc.type=BT_GATT_DISCOVER_PRIMARY;
 int e=bt_gatt_discover(c,&disc);if(e)bt_conn_disconnect(c,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}
static void disconnected(struct bt_conn *c,uint8_t reason){printk("Disconnected reason=%u\n",reason);k_spinlock_key_t key=k_spin_lock(&sample_lock);latest_len=0;memset(latest,0,sizeof(latest));k_spin_unlock(&sample_lock,key);if(peer){bt_conn_unref(peer);peer=NULL;}k_work_reschedule(&scan_work,K_SECONDS(1));}
static void phy(struct bt_conn *c,struct bt_conn_le_phy_info *p){printk("PHY tx=%u rx=%u\n",p->tx_phy,p->rx_phy);}
BT_CONN_CB_DEFINE(callbacks)={.connected=connected,.disconnected=disconnected,.le_phy_updated=phy};
static bool match(struct bt_data *d,void *u){
 if(d->type==BT_DATA_UUID16_ALL||d->type==BT_DATA_UUID16_SOME){
  for(int i=0;i+1<d->data_len;i+=2){
   if(d->data[i]==0xf0&&d->data[i+1]==0xff){
    *(bool*)u=true;
    return false;
   }
  }
 }
 return true;
}
static void found(const bt_addr_le_t *addr,int8_t rssi,uint8_t type,struct net_buf_simple *ad){
 if(peer)return;
 bool yes=false;
 bt_data_parse(ad,match,&yes);
 if(!yes)return;
 bt_le_scan_stop();
 int e=bt_conn_le_create(addr,BT_CONN_LE_CREATE_CONN,BT_LE_CONN_PARAM(6,6,0,400),&peer);
 printk("Create result=%d RSSI=%d\n",e,rssi);
 if(e)k_work_reschedule(&scan_work,K_SECONDS(1));
}
static void scan(void){int e=bt_le_scan_start(BT_LE_SCAN_ACTIVE,found);printk("Scan result=%d\n",e);}
int main(void){
 uint32_t lo=NRF_FICR->DEVICEADDR[0],hi=NRF_FICR->DEVICEADDR[1];
 for(int i=0;i<4;i++)anchor_identity.a.val[i]=(lo>>(8*i))&0xff;
 anchor_identity.a.val[4]=hi&0xff;anchor_identity.a.val[5]=((hi>>8)&0x3f)|0xc0;anchor_identity.type=BT_ADDR_LE_RANDOM;
 bt_id_create(&anchor_identity,NULL);
 uart_irq_callback_user_data_set(console,serial_irq,NULL);uart_irq_rx_enable(console);k_sem_init(&at_ready,0,1);
 printk("ANCHOR USB READY\n");
 printk("BUILD: ANCHOR_UART_AT_RXPOOL_AUTO_START_VCCLOW_LEDHEARTBEAT\n");
 int vcc_init=vcc_enable_init();
 int led_result=led_init();
 printk("GPIO init P0.13 LOW (VCC ON)=%d P0.15 LED=%d\n",vcc_init,led_result);
 int en_init=bt_enable_init();
 printk("BT_EN P0.02 input init=%d\n",en_init);
 if(!en_init){bt_enable_level=bt_enable_read();printk("BT_EN P0.02=%s (%d)\n",bt_enable_level?"HIGH":"LOW",bt_enable_level);}
 int uart_result=wire_start();
 int64_t next_uart_retry=k_uptime_get()+500;
 int64_t next_led=k_uptime_get()+500;int led_level=0;int64_t next=0;while(1){commands();if(k_uptime_get()>=next_led){led_level=!led_level;int le=led_set(led_level);if(le)printk("LED P0.15 set error=%d\\n",le);next_led=k_uptime_get()+500;}if(atomic_get(&uart_enabled))uart_commands();if(!atomic_get(&uart_enabled)&&uart_start_retries<5&&k_uptime_get()>=next_uart_retry){uart_start_retries++;uart_result=wire_start();next_uart_retry=k_uptime_get()+500;printk("UART startup retry=%u result=%d\n",uart_start_retries,uart_result);}if(start_requested&&!started){start_requested=false;printk("BT starting\n");int e=bt_enable(NULL);printk("BT init=%d\n",e);if(!e){started=true;scan();}}if(k_uptime_get()>=next){printk("STATS packets=%ld bytes=%ld\n",(long)atomic_get(&packets),(long)atomic_get(&bytes));uint16_t ch[8];size_t len;k_spinlock_key_t key=k_spin_lock(&sample_lock);memcpy(ch,latest,sizeof(ch));len=latest_len;k_spin_unlock(&sample_lock,key);
 printk("FRAMES valid=%ld invalid=%ld boot=%ld len=%u channels=%u,%u,%u,%u,%u,%u,%u,%u\n",(long)atomic_get(&valid),(long)atomic_get(&invalid),(long)atomic_get(&boot),(unsigned)len,ch[0],ch[1],ch[2],ch[3],ch[4],ch[5],ch[6],ch[7]);printk("UART done=%ld abort=%ld error=%ld skipped=%ld\n",(long)atomic_get(&uart_done),(long)atomic_get(&uart_abort),(long)atomic_get(&uart_error),(long)atomic_get(&uart_skipped));int en=bt_enable_read();if(en>=0&&en!=bt_enable_level){bt_enable_level=en;printk("BT_EN P0.02=%s (%d)\n",en?"HIGH":"LOW",en);}next=k_uptime_get()+2000;}k_sleep(K_MSEC(10));}
}
