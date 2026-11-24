#include "../firmware/trainer_protocol.h"
#include <cassert>
#include <cstdint>
#include <vector>
#include <iostream>

static std::vector<uint8_t> frame(const uint8_t channels[12]) {
  std::vector<uint8_t> raw{0x80};
  for (int i=0;i<12;i++) raw.push_back(channels[i]);
  uint8_t crc = 0;
  for (auto b: raw) crc ^= b;
  raw.push_back(crc);
  std::vector<uint8_t> out{0x7e};
  for (auto b: raw) {
    if (b==0x7e || b==0x7d) {out.push_back(0x7d);out.push_back(b^0x20);}
    else out.push_back(b);
  }
  out.push_back(0x7e);
  return out;
}
int main() {
  TrainerFrameParser p;
  const uint8_t center[12]={0};
  auto f=frame(center);
  int accepted=0;
  for (auto b:f) if (p.feed(b)==TrainerFrameParser::Valid) ++accepted;
  assert(accepted==1 && p.wireLen==f.size());
  uint8_t escaped[12]={0x7e,0x7d,0x00,0x10,0x7d,0,0,0,0,0,0,0};
  f=frame(escaped);p.reset();accepted=0;
  for(auto b:f) if(p.feed(b)==TrainerFrameParser::Valid) ++accepted;
  assert(accepted==1 && p.wireLen==f.size());
  f=frame(center);f[3]^=0x01;p.reset();accepted=0;
  for(auto b:f) if(p.feed(b)==TrainerFrameParser::Valid) ++accepted;
  assert(accepted==0);
  f=frame(center);p.reset();accepted=0;
  for(auto b:f) { if(p.feed(b)==TrainerFrameParser::Valid)++accepted; }
  for(auto b:f) { if(p.feed(b)==TrainerFrameParser::Valid)++accepted; }
  assert(accepted==2);
  p.reset();for(int i=0;i<120;i++)p.feed(0x12);
  assert(p.wireLen==0);
  std::cout << "FrSky trainer frame parser: all tests passed\n";
}
