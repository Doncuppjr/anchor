#ifndef TRAINER_H
#define TRAINER_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
/* One complete legacy or escaped PARA frame per notification. */
static inline bool trainer_decode(const uint8_t *p,size_t n,uint16_t out[8]){
 if(n<16||n>30||p[0]!=0x7e||p[n-1]!=0x7e)return false;
 uint8_t d[13],crc=0;size_t i=1;
 for(size_t j=0;j<13;j++){
  if(i>=n-1)return false;
  uint8_t b=p[i++];
  if(b==0x7d){if(i>=n-1)return false;b=p[i++]^0x20;if(b!=0x7d&&b!=0x7e)return false;}
  else if(b==0x7e)return false;
  d[j]=b;crc^=b;
 }
 if(d[0]!=0x80)return false;
 /* TinyGo emits checksum literally, including 7d or 7e. */
 if(i+1==n-1){if(p[i]!=crc)return false;}
 else if(i+2==n-1){if(p[i]!=0x7d||(p[i+1]^0x20)!=crc||(crc!=0x7d&&crc!=0x7e))return false;}
 else return false;
 for(size_t j=0;j<4;j++){
  const uint8_t *q=d+1+j*3;
  out[j*2]=q[0]|((uint16_t)(q[1]&0xf0)<<4);
  out[j*2+1]=((uint16_t)(q[1]&15)<<4)|(q[2]>>4)|((uint16_t)(q[2]&15)<<8);
 }
 return true;
}
#endif
