#include "health_replay_engine.h"
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc,char** argv) {
  if(argc!=3) { std::cerr<<"usage: health_replay decoded.bin expected-source-sha256\n"; return 2; }
  std::ifstream input(argv[1],std::ios::binary);
  if(!input) { std::cerr<<"cannot open recording\n"; return 2; }
  HealthReplay replay(argv[2]);
  for(;;) {
    uint8_t header[MotionCapture::header_size]; input.read(reinterpret_cast<char*>(header),sizeof(header));
    if(input.gcount()==0 && input.eof()) break;
    if(input.gcount()!=sizeof(header)) { std::cerr<<"truncated record header\n"; return 1; }
    const size_t payload=size_t(header[1])|(size_t(header[2])<<8);
    if(payload>MotionCapture::checkpoint_capacity+160) { std::cerr<<"oversized record\n"; return 1; }
    std::vector<uint8_t> record(header,header+sizeof(header)); record.resize(sizeof(header)+payload);
    input.read(reinterpret_cast<char*>(record.data()+sizeof(header)),payload);
    if(size_t(input.gcount())!=payload) { std::cerr<<"truncated record payload\n"; return 1; }
    if(!replay.feed(record.data(),record.size())) { std::cerr<<replay.failure()<<'\n'; return 1; }
  }
  if(!replay.finish()) { std::cerr<<replay.failure()<<'\n'; return 1; }
  std::cout<<"Replay verified: "<<replay.summary()<<'\n'; return 0;
}
