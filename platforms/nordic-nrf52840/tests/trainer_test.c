#include "../src/trainer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void push(uint8_t *b,size_t *n,uint8_t x){
 if(x==0x7e||x==0x7d){b[(*n)++]=0x7d;x^=0x20;}b[(*n)++]=x;
}
int main(void){
 unsigned special=0;
 for(unsigned k=0;k<100000;k++){
 uint16_t in[8],out[8];uint8_t b[32],d[13];size_t n=0;
 d[0]=0x80;
 for(unsigned j=0;j<8;j++)in[j]=(k*(17+j*13)+j*125)&4095;
 for(unsigned j=0;j<4;j++){unsigned a=in[2*j],c=in[2*j+1];d[1+3*j]=a;d[2+3*j]=((a>>4)&0xf0)|((c>>4)&15);d[3+3*j]=((c&15)<<4)|(c>>8);}
 uint8_t crc=0;b[n++]=0x7e;
 for(unsigned j=0;j<13;j++){crc^=d[j];push(b,&n,d[j]);}
 size_t prefix=n;
 b[n++]=crc;b[n++]=0x7e;
 assert(trainer_decode(b,n,out));assert(!memcmp(in,out,sizeof(in)));
 b[n-2]^=1;assert(!trainer_decode(b,n,out));
 n=prefix;push(b,&n,crc);b[n++]=0x7e;
 assert(trainer_decode(b,n,out));assert(!memcmp(in,out,sizeof(in)));
 if(crc==0x7d||crc==0x7e)special++;
 assert(!trainer_decode(b,n-1,out));
 }
 printf("PASS 100000 vectors, legacy and escaped checksum, %u reserved checksums, corruption and truncation rejected\n",special);
}
