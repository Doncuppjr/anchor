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
#include <hal/nrf_gpio.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/drivers/pwm.h>
#include <string.h>
#include <errno.h>
#include "trainer.h"
static uint32_t startup_gp, startup_gp2, startup_reset;
static int capture_boot_regs(void){startup_gp=NRF_POWER->GPREGRET;startup_gp2=NRF_POWER->GPREGRET2;startup_reset=NRF_POWER->RESETREAS;NRF_POWER->RESETREAS=startup_reset;return 0;}
SYS_INIT(capture_boot_regs, PRE_KERNEL_1, 0);
static const struct device *console=DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static const struct device *gpio0=DEVICE_DT_GET(DT_NODELABEL(gpio0));
static int bt_enable_level=-1;
static int heartbeat_result=-1;
#if defined(CONFIG_BOARD_XIAO_BLE)
static int vcc_enable_init(void){return 0;}
static int vcc_set(int level){ARG_UNUSED(level);return -ENOTSUP;}
static int vcc_level(void){return -ENOTSUP;}
#else
static int vcc_enable_init(void){if(!device_is_ready(gpio0))return -ENODEV;return gpio_pin_configure(gpio0,13,GPIO_OUTPUT_ACTIVE);}
static int vcc_set(int level){return gpio_pin_set(gpio0,13,level);}
static int vcc_level(void){return (NRF_P0->OUT>>13)&1;}
#endif
#if defined(CONFIG_BOARD_XIAO_BLE)
static const struct device *led_pwm=DEVICE_DT_GET(DT_NODELABEL(pwm0));
#else
static const struct gpio_dt_spec status_led=GPIO_DT_SPEC_GET(DT_ALIAS(led0),gpios);
#endif
static atomic_t link_state, stream_ready, sleep_pending, wake_pulse, update_led, error_led;
#if defined(CONFIG_BOARD_XIAO_BLE)
#define LED_PERIOD PWM_USEC(1000)
static void led_pwm_set(unsigned channel,unsigned level){
 pwm_set(led_pwm,channel,LED_PERIOD,LED_PERIOD*level/255,PWM_POLARITY_INVERTED);
}
static unsigned breathe(uint32_t now,unsigned period,unsigned peak){
 unsigned phase=now%period, half=period/2;
 return peak*(phase>half?period-phase:phase)/half;
}
static int led_init(void){return device_is_ready(led_pwm)?0:-ENODEV;}
static void leds_tick(void){
 uint32_t now=k_uptime_get_32();int state=atomic_get(&link_state);
 unsigned red=0,green=0,blue=breathe(now,3000,0x8f);
 if(state==1)green=breathe(now,300,0x8f);
 else if(state==3)green=breathe(now,100,0x8f);
 else if(state==2)green=255;
 if(atomic_get(&update_led))red=breathe(now,100,0x4f);
 else if(atomic_get(&sleep_pending)||atomic_get(&error_led))red=breathe(now,300,0x4f);
 else if(atomic_get(&wake_pulse))red=0x4f;
 led_pwm_set(0,red);led_pwm_set(1,green);led_pwm_set(2,blue);
}
static void leds_off(void){for(unsigned i=0;i<3;i++)led_pwm_set(i,0);}
#else
static int led_init(void){if(!gpio_is_ready_dt(&status_led))return -ENODEV;return gpio_pin_configure_dt(&status_led,GPIO_OUTPUT_INACTIVE);}
static void leds_tick(void){gpio_pin_set_dt(&status_led,(k_uptime_get_32()/500)%2);}
#endif
static int board_power_init(void){
 int e=vcc_enable_init();if(e)return e;
 e=led_init();
 return e;
}
SYS_INIT(board_power_init,POST_KERNEL,90);
#define printk(...) do {uint32_t dtr=0;if(device_is_ready(console)&&!uart_line_ctrl_get(console,UART_LINE_CTRL_DTR,&dtr)&&dtr)printk(__VA_ARGS__);} while(0)
static int bt_enable_init(void){if(!device_is_ready(gpio0))return -ENODEV;return gpio_pin_configure(gpio0,2,GPIO_INPUT | GPIO_PULL_UP);}
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
static uint8_t update_selector=0x59;
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
  if(!atomic_get(&uart_enabled)||!atomic_get(&stream_ready))continue;
  k_spinlock_key_t key=k_spin_lock(&sample_lock);
  size_t n=latest_len;uint32_t seq=generation;
  if(n&&seq!=consumed)memcpy(dma_frame,latest_wire,n);
  k_spin_unlock(&sample_lock,key);
  if(!n||seq==consumed)continue;
  if(consumed&&seq-consumed>1)atomic_add(&uart_skipped,seq-consumed-1);
  consumed=seq;
  k_mutex_lock(&uart_tx_mutex,K_FOREVER);if(!atomic_get(&stream_ready)){k_mutex_unlock(&uart_tx_mutex);continue;}k_sem_reset(&tx_finished);
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
static atomic_t central_active, link_pending, lost_pending, discovery_requested;
static char peer_mac[13], selected_mac[13], found_mac[13];
static int64_t scan_after, discovery_until, sleep_at, wake_until;
static unsigned en_transitions, sleep_attempts;
static int bt_disable_result=-ENODATA;
static const char *sleep_state="idle";
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
 k_mutex_lock(&uart_tx_mutex,K_FOREVER);memcpy(tx,s,n);tx[n++]='\r';tx[n++]='\n';
 k_sem_reset(&tx_finished);
 int e=uart_tx(wire_uart,tx,n,SYS_FOREVER_US);
 if(!e)k_sem_take(&tx_finished,K_FOREVER);
 k_mutex_unlock(&uart_tx_mutex);
}
static void mac_format(const bt_addr_le_t *addr,char out[13]){
 snprintf(out,13,"%02X%02X%02X%02X%02X%02X",addr->a.val[5],addr->a.val[4],addr->a.val[3],addr->a.val[2],addr->a.val[1],addr->a.val[0]);
}
static void address_reply(const char *prefix,const char *mac){
 char line[64];snprintf(line,sizeof(line),"%s%s",prefix,mac);uart_reply(line);
}
static void at_identity(const char *prefix){char mac[13];mac_format(&anchor_identity,mac);address_reply(prefix,mac);}
static void stop_link(void){
 atomic_clear(&central_active);atomic_clear(&stream_ready);atomic_clear(&link_state);
 atomic_clear(&link_pending);atomic_clear(&lost_pending);atomic_clear(&discovery_requested);
 k_work_cancel_delayable(&scan_work);
 if(started){bt_le_scan_stop();if(peer)bt_conn_disconnect(peer,BT_HCI_ERR_REMOTE_USER_TERM_CONN);}
 k_spinlock_key_t key=k_spin_lock(&sample_lock);latest_len=0;k_spin_unlock(&sample_lock,key);
}
static void reset_host(void){
 stop_link();role_active=false;update_authorized=false;update_selector=0x59;start_requested=false;
 at_line_len=0;k_msgq_purge(&at_q);
 struct uart_config cfg={.baudrate=57600,.parity=UART_CFG_PARITY_NONE,.stop_bits=UART_CFG_STOP_BITS_1,.data_bits=UART_CFG_DATA_BITS_8,.flow_ctrl=UART_CFG_FLOW_CTRL_NONE};
 k_mutex_lock(&uart_tx_mutex,K_FOREVER);
 int e=uart_configure(wire_uart,&cfg);if(!e)atomic_set(&at_baud,57600);
 k_mutex_unlock(&uart_tx_mutex);printk("BT disabled; baud reset=%d\n",e);
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
 else if(!strncmp(s,"AT+TXPW",7)){char line[32];snprintf(line,sizeof(line),"OK+Txpw:%.23s",s+7);uart_reply(line);uart_reply("OK+CAPS:1,UPDATE_UART,UPDATE_UART460800");at_identity("Central:");}
 else if(!strcmp(s,"AT+UPDATE=UART")||!strcmp(s,"AT+UPDATE=UART,460800")){
  if(role_active){uart_reply("ERROR");return;}
  update_selector=!strcmp(s,"AT+UPDATE=UART,460800")?0x5A:0x59;
  update_authorized=true;
  uart_reply(update_selector==0x5A?"OK+UPDATE:UART,460800":"OK+UPDATE:UART");
 }
 else if(!strcmp(s,"AT+DOWNLOAD")){if(!update_authorized){uart_reply("ERROR");return;}uart_reply("OK+DOWNLOAD");atomic_set(&update_led,1);leds_tick();k_sleep(K_MSEC(100));NRF_POWER->GPREGRET=update_selector;sys_reboot(SYS_REBOOT_COLD);}
 else if(!strcmp(s,"AT+ROLE1")){
  uart_reply("OK+Role:1");at_identity("Central:");role_active=true;
  atomic_set(&central_active,1);scan_after=k_uptime_get()+300;
  if(!started){int e=bt_enable(NULL);printk("AT BLE init=%d\n",e);if(!e)started=true;}
 }
 else if(!strcmp(s,"AT+ROLE0")){stop_link();uart_reply("OK+Role:0");at_identity("Peripheral:");role_active=true;}
 else if(!strcmp(s,"AT+DISC?")){
  uart_reply("OK+DISCS");if(peer_mac[0]&&atomic_get(&link_state)==2)address_reply("OK+DISC:",peer_mac);
  else {found_mac[0]=0;atomic_set(&discovery_requested,1);discovery_until=k_uptime_get()+3500;scan_after=k_uptime_get();}
 }
 else if(!strncmp(s,"AT+CON",6)){
  const char *mac=s+6;if(strlen(mac)!=12||strspn(mac,"0123456789ABCDEFabcdef")!=12){uart_reply("ERROR");return;}
  for(int i=0;i<12;i++)selected_mac[i]=(mac[i]>='a'&&mac[i]<='f')?mac[i]-32:mac[i];selected_mac[12]=0;
  uart_reply("OK+CONNA");atomic_clear(&discovery_requested);
  if(atomic_get(&link_state)==2&&!strcmp(peer_mac,selected_mac)){atomic_clear(&stream_ready);atomic_set(&link_pending,1);}
  else {if(peer)bt_conn_disconnect(peer,BT_HCI_ERR_REMOTE_USER_TERM_CONN);scan_after=k_uptime_get()+300;}
 }
 else if(!strcmp(s,"AT+CLEAR")){selected_mac[0]=0;stop_link();uart_reply("OK+CLEAR");}
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
static void sleep_report(void){
 uint32_t dtr=0;int rc=uart_line_ctrl_get(console,UART_LINE_CTRL_DTR,&dtr);
 int64_t remaining=sleep_at-k_uptime_get();
 printk("SLEEP EN_RAW=%d EN_TRANSITIONS=%u PIN_CNF=%08x GPIO_IN=%08x LATCH=%08x USBREGSTATUS=%08x DTR=%u DTR_RC=%d\n",bt_enable_read(),en_transitions,(unsigned)NRF_P0->PIN_CNF[2],(unsigned)NRF_P0->IN,(unsigned)NRF_P0->LATCH,(unsigned)NRF_POWER->USBREGSTATUS,(unsigned)dtr,rc);
 printk("SLEEP STATE=%s REMAINING_MS=%d ATTEMPTS=%u BT_DISABLE_RC=%d PEER=%d ROLE=%d DFU_AUTH=%d STARTED=%d RESETREAS=%08x OFF_WAKE=%d RETAINED=%02x\n",sleep_state,(int)(sleep_at&&remaining>0?remaining:0),sleep_attempts,bt_disable_result,peer!=NULL,role_active,update_authorized,started,(unsigned)startup_reset,!!(startup_reset&POWER_RESETREAS_OFF_Msk),(unsigned)startup_gp2);
}
static void commands(void){
 static char b[48];static unsigned n;uint8_t c;
 while(ring_buf_get(&rx,&c,1)){
 if(c==10||c==13){if(n){b[n]=0;if(!strcmp(b,"UART_DFU")||!strcmp(b,"AT+DOWNLOAD")){printk("UART DFU reboot\n");k_sleep(K_MSEC(100));NRF_POWER->GPREGRET=0x59;sys_reboot(SYS_REBOOT_COLD);}else if(!strcmp(b,"UF2")){printk("UF2 reboot\n");k_sleep(K_MSEC(100));NRF_POWER->GPREGRET=0x57;sys_reboot(SYS_REBOOT_COLD);}else if(!strcmp(b,"TESTBAUD57600")){if(atomic_get(&uart_enabled)){struct uart_config c={.baudrate=57600,.parity=UART_CFG_PARITY_NONE,.stop_bits=UART_CFG_STOP_BITS_1,.data_bits=UART_CFG_DATA_BITS_8,.flow_ctrl=UART_CFG_FLOW_CTRL_NONE};int e=uart_configure(wire_uart,&c);atomic_set(&at_baud,e?115200:57600);printk("UART test baud=%ld result=%d\n",(long)atomic_get(&at_baud),e);}}else if(!strcmp(b,"UART_START")){uart_start_retries=5;wire_start();}
 else if(!strcmp(b,"VCC_LOW")){int e=vcc_set(0);printk("VCC P0.13=LOW (VCC OFF) result=%d readback=%d\\n",e,vcc_level());}
 else if(!strcmp(b,"VCC_HIGH")){int e=vcc_set(1);printk("VCC P0.13=HIGH (VCC ON) result=%d readback=%d\\n",e,vcc_level());}else if(!strcmp(b,"BT_START")){role_active=true;start_requested=true;}else if(!strcmp(b,"SLEEP?")){sleep_report();}else if(!strcmp(b,"PING")){int en=bt_enable_read();int vcc=vcc_level();printk("PONG ANCHOR GPREGRET=%02x GPREGRET2=%02x START=%02x/%02x MARK=%08x BT_EN_P0.02=%s(%d) VCC_EN_P0.13=%s(%d) RESET=%08x LED=%d\n",(unsigned)NRF_POWER->GPREGRET,(unsigned)NRF_POWER->GPREGRET2,(unsigned)startup_gp,(unsigned)startup_gp2,IS_ENABLED(CONFIG_BOARD_XIAO_BLE)?0U:(unsigned)(*((volatile uint32_t*)0x20007F78)),en>0?"HIGH":en==0?"LOW":"ERROR",en,vcc>0?"HIGH":vcc==0?"LOW":"ERROR",vcc,(unsigned)startup_reset,heartbeat_result);}n=0;}}
 else if(n<sizeof(b)-1)b[n++]=c;else n=0;
 }}
static uint8_t notify(struct bt_conn *c,struct bt_gatt_subscribe_params *p,const void *data,uint16_t len){
 if(!data)return BT_GATT_ITER_STOP;
 if(!atomic_get(&central_active))return BT_GATT_ITER_CONTINUE;
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
 err=bt_gatt_subscribe(c,&sub);if(!err&&atomic_get(&central_active)){atomic_set(&link_state,2);atomic_set(&link_pending,1);}printk("Subscribe result=%d value=%u ccc=%u\n",err,sub.value_handle,sub.ccc_handle);
 }
 if(err){printk("GATT error=%d\n",err);bt_conn_disconnect(c,BT_HCI_ERR_REMOTE_USER_TERM_CONN);}
 return BT_GATT_ITER_STOP;
}
static void connected(struct bt_conn *c,uint8_t err){
 if(err){printk("Connection failed=%u\n",err);bt_conn_unref(peer);peer=NULL;k_work_reschedule(&scan_work,K_SECONDS(1));return;}
 if(!atomic_get(&central_active)){bt_conn_disconnect(c,BT_HCI_ERR_REMOTE_USER_TERM_CONN);return;}
 mac_format(bt_conn_get_dst(c),peer_mac);
 printk("Connected BLE\n");
 struct bt_conn_info i;if(!bt_conn_get_info(c,&i))printk("interval_us=%u latency=%u\n",i.le.interval_us,i.le.latency);
 
 memset(&disc,0,sizeof(disc));memset(&sub,0,sizeof(sub));
 disc.uuid=NULL;disc.func=discover;disc.start_handle=1;disc.end_handle=0xffff;disc.type=BT_GATT_DISCOVER_PRIMARY;
 int e=bt_gatt_discover(c,&disc);if(e)bt_conn_disconnect(c,BT_HCI_ERR_REMOTE_USER_TERM_CONN);
}
static void disconnected(struct bt_conn *c,uint8_t reason){printk("Disconnected reason=%u\n",reason);k_spinlock_key_t key=k_spin_lock(&sample_lock);latest_len=0;memset(latest,0,sizeof(latest));k_spin_unlock(&sample_lock,key);if(peer){bt_conn_unref(peer);peer=NULL;}if(atomic_get(&stream_ready))atomic_set(&lost_pending,1);atomic_clear(&stream_ready);atomic_clear(&link_pending);atomic_clear(&link_state);if(atomic_get(&central_active))k_work_reschedule(&scan_work,K_SECONDS(1));}
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
 if(peer||!atomic_get(&central_active))return;
 bool yes=false;
 bt_data_parse(ad,match,&yes);
 if(!yes)return;
 char mac[13];mac_format(addr,mac);
 if(atomic_get(&discovery_requested)){memcpy(found_mac,mac,13);return;}
 if(selected_mac[0]&&strcmp(selected_mac,mac))return;
 bt_le_scan_stop();
 int e=bt_conn_le_create(addr,BT_CONN_LE_CREATE_CONN,BT_LE_CONN_PARAM(6,6,0,400),&peer);
 printk("Create result=%d RSSI=%d\n",e,rssi);
 if(e)k_work_reschedule(&scan_work,K_SECONDS(1));else atomic_set(&link_state,3);
}
static void scan(void){
 if(!started||!atomic_get(&central_active)||peer)return;
 int e=bt_le_scan_start(BT_LE_SCAN_ACTIVE,found);if(!e||e==-EALREADY)atomic_set(&link_state,1);
 printk("Scan result=%d\n",e);
}
static void host_poll(void){
 int en=bt_enable_read();
 if(en>=0&&en!=bt_enable_level){bt_enable_level=en;en_transitions++;sleep_state=en?"countdown":"idle";printk("BT_EN P0.02=%s (%d)\n",en?"HIGH":"LOW",en);if(en){reset_host();sleep_at=k_uptime_get()+2500;atomic_set(&sleep_pending,1);}else{sleep_at=0;atomic_clear(&sleep_pending);}}
 if(atomic_get(&lost_pending)&&atomic_cas(&lost_pending,1,0))uart_reply("DisConnected");
 if(atomic_get(&link_pending)&&atomic_cas(&link_pending,1,0)&&atomic_get(&central_active)){
  address_reply("Connected:",peer_mac);atomic_set(&stream_ready,1);k_sem_give(&frame_ready);
 }
 if(atomic_get(&discovery_requested)&&found_mac[0]){address_reply("OK+DISC:",found_mac);found_mac[0]=0;atomic_clear(&discovery_requested);bt_le_scan_stop();atomic_clear(&link_state);uart_reply("OK+DISCE");}
 else if(atomic_get(&discovery_requested)&&k_uptime_get()>=discovery_until){atomic_clear(&discovery_requested);bt_le_scan_stop();atomic_clear(&link_state);uart_reply("OK+DISCE");}
 if(scan_after&&k_uptime_get()>=scan_after){scan_after=0;scan();}
 if(wake_until&&k_uptime_get()>=wake_until){wake_until=0;atomic_clear(&wake_pulse);}
#if defined(CONFIG_BOARD_XIAO_BLE)
 if(sleep_at&&k_uptime_get()>=sleep_at&&en==1&&!update_authorized&&!peer){
  /* Only an open USB diagnostic session inhibits sleep, not supply voltage. */
  uint32_t dtr=0;
  if((NRF_POWER->USBREGSTATUS & POWER_USBREGSTATUS_VBUSDETECT_Msk) && !uart_line_ctrl_get(console,UART_LINE_CTRL_DTR,&dtr) && dtr){
   sleep_state="usb_dtr";atomic_clear(&sleep_pending);
  }else{
   sleep_attempts++;sleep_state="shutdown";
   if(started){int e=bt_disable();bt_disable_result=e;if(e){atomic_set(&error_led,1);sleep_at=k_uptime_get()+2500;return;}started=false;}
   if(bt_enable_read()!=1){sleep_at=0;atomic_clear(&sleep_pending);return;}
   atomic_clear(&uart_enabled);uart_rx_disable(wire_uart);
   leds_off();
   /* Stop PWM and hold the active-low LEDs off through System OFF. */
   NRF_PWM0->TASKS_STOP=1;
   NRF_PWM0->ENABLE=0;
   nrf_gpio_pin_set(26);nrf_gpio_cfg_output(26);
   nrf_gpio_pin_set(30);nrf_gpio_cfg_output(30);
   nrf_gpio_pin_set(6);nrf_gpio_cfg_output(6);
   nrf_gpio_cfg_sense_input(2,NRF_GPIO_PIN_PULLUP,NRF_GPIO_PIN_SENSE_LOW);
   NRF_POWER->GPREGRET2=0xA6;
   sys_poweroff();
  }
 }
#endif
}

int main(void){
 heartbeat_result=led_init();
 uint32_t lo=NRF_FICR->DEVICEADDR[0],hi=NRF_FICR->DEVICEADDR[1];
 for(int i=0;i<4;i++)anchor_identity.a.val[i]=(lo>>(8*i))&0xff;
 anchor_identity.a.val[4]=hi&0xff;anchor_identity.a.val[5]=((hi>>8)&0x3f)|0xc0;anchor_identity.type=BT_ADDR_LE_RANDOM;
 bt_id_create(&anchor_identity,NULL);
 uart_irq_callback_user_data_set(console,serial_irq,NULL);uart_irq_rx_enable(console);k_sem_init(&at_ready,0,1);
 printk("ANCHOR USB READY\n");
 printk("BUILD: ANCHOR_UART_AT_RXPOOL_AUTO_START_XIAO_PWM_LED_SYSTEM_OFF\n");
 printk("EARLY_GPIO reset=%08x board-vcc-control=%d\n",(unsigned)startup_reset,vcc_level());
 int en_init=bt_enable_init();
 printk("BT_EN P0.02 input init=%d\n",en_init);
 if(!en_init){bt_enable_level=bt_enable_read();printk("BT_EN P0.02=%s (%d)\n",bt_enable_level?"HIGH":"LOW",bt_enable_level);}
 if(startup_reset & POWER_RESETREAS_OFF_Msk){atomic_set(&wake_pulse,1);wake_until=k_uptime_get()+500;}
 if(bt_enable_level==1){sleep_state="countdown";sleep_at=k_uptime_get()+2500;atomic_set(&sleep_pending,1);}
 int uart_result=wire_start();
 int64_t next_uart_retry=k_uptime_get()+500;
 int64_t next=0;while(1){host_poll();if(!heartbeat_result)leds_tick();commands();if(atomic_get(&uart_enabled))uart_commands();if(!atomic_get(&uart_enabled)&&uart_start_retries<5&&k_uptime_get()>=next_uart_retry){uart_start_retries++;uart_result=wire_start();next_uart_retry=k_uptime_get()+500;printk("UART startup retry=%u result=%d\n",uart_start_retries,uart_result);}if(start_requested&&!started){start_requested=false;printk("BT starting\n");int e=bt_enable(NULL);printk("BT init=%d\n",e);if(!e){started=true;scan();}}if(k_uptime_get()>=next){printk("STATS packets=%ld bytes=%ld\n",(long)atomic_get(&packets),(long)atomic_get(&bytes));uint16_t ch[8];size_t len;k_spinlock_key_t key=k_spin_lock(&sample_lock);memcpy(ch,latest,sizeof(ch));len=latest_len;k_spin_unlock(&sample_lock,key);
 printk("FRAMES valid=%ld invalid=%ld boot=%ld len=%u channels=%u,%u,%u,%u,%u,%u,%u,%u\n",(long)atomic_get(&valid),(long)atomic_get(&invalid),(long)atomic_get(&boot),(unsigned)len,ch[0],ch[1],ch[2],ch[3],ch[4],ch[5],ch[6],ch[7]);printk("UART done=%ld abort=%ld error=%ld skipped=%ld\n",(long)atomic_get(&uart_done),(long)atomic_get(&uart_abort),(long)atomic_get(&uart_error),(long)atomic_get(&uart_skipped));next=k_uptime_get()+2000;}k_sleep(K_MSEC(10));}
}
