#include "kern_tgl_orphan_stamp.hpp"
#include <assert.h>
#include <vector>
using namespace TGLOrphanStamp;
using O=Outcome;

struct Machine {
 alignas(8) uint8_t bytes[0x400] {};
 std::vector<uint32_t> hw;
 std::vector<volatile uint32_t *> addresses;
 explicit Machine(int count) : hw(count), addresses(count) {
  for (int i=0;i<count;++i) addresses[i]=&hw[i];
  put<volatile uint32_t **>(stampAddresses,addresses.data());
  put<int32_t>(numStamps,count);
  put<int32_t>(restartIndex,count);  // idle: restart_channel leaves index = numStamps, type 0
 }
 template<typename T> void put(size_t o,T v) { memcpy(bytes+o,&v,sizeof(v)); }
 void submitted(int i,uint32_t v) { put<uint32_t>(progressBase+i*progressStride+submittedField,v); }
 void restart(int i) { put<int32_t>(restartIndex,i);put<int32_t>(restartType,restartTypeChannel); }
};

int main() {
 // Layout offsets agree with the family code they are checked against.
 assert(checkGPUProgressCode[0x1B]==numStamps && checkGPUProgressCode[0x3A]==progressBase);
 assert(checkGPUProgressCode[0x44]==stampAddresses && checkGPUProgressCode[0x4F]==submittedField);
 assert(signalStampCode[0x13]==stampAddresses);
 assert(deviceTerminatedCode[0x17]==((progressBase+submittedField)&0xFF) && deviceTerminatedCode[0x18]==((progressBase+submittedField)>>8));
 assert(deviceTerminatedCode[0x37]==progressStride);
 assert(deviceTerminatedCode[0x3D]==0x0F && deviceTerminatedCode[0x3F]==0xF8);  // sfence
 // layoutKnown wants all three functions exact.
 uint8_t a[sizeof(checkGPUProgressCode)],b[sizeof(signalStampCode)],c[sizeof(deviceTerminatedCode)];
 memcpy(a,checkGPUProgressCode,sizeof(a));memcpy(b,signalStampCode,sizeof(b));memcpy(c,deviceTerminatedCode,sizeof(c));
 assert(layoutKnown(a,b,c));
 a[0x3A]^=8;assert(!layoutKnown(a,b,c));a[0x3A]^=8;
 b[0x13]^=8;assert(!layoutKnown(a,b,c));b[0x13]^=8;
 c[0x37]^=8;assert(!layoutKnown(a,b,c));c[0x37]^=8;
 assert(!layoutKnown(nullptr,b,c) && !layoutKnown(a,nullptr,c) && !layoutKnown(a,b,nullptr));

 Machine m(64);
 m.hw[10]=0x100;m.submitted(10,0x105);
 int channel=0;
 // Not inside restart_channel for this index (e.g. zero_vidmem): never touched.
 assert(retire(m.bytes,10,nullptr,reasonNoContext,true).outcome==O::NotRestart && m.hw[10]==0x100);
 m.restart(11);
 assert(retire(m.bytes,10,nullptr,reasonNoContext,true).outcome==O::NotRestart && m.hw[10]==0x100);
 m.put<int32_t>(restartIndex,10);m.put<int32_t>(restartType,1);
 assert(retire(m.bytes,10,nullptr,reasonNoContext,true).outcome==O::NotRestart && m.hw[10]==0x100);
 m.restart(10);
 // A real hang (channel returned) or any other donor reason: native path only.
 assert(retire(m.bytes,10,&channel,reasonNoContext,true).outcome==O::Channel && m.hw[10]==0x100);
 for (uint32_t r:{1u,2u,3u,5u,6u,UINT32_MAX})
  assert(retire(m.bytes,10,nullptr,r,true).outcome==O::OtherReason && m.hw[10]==0x100);
 // Unknown family layout: no write.
 assert(retire(m.bytes,10,nullptr,reasonNoContext,false).outcome==O::Layout && m.hw[10]==0x100);
 // Orphan: returned catches up with submitted, nothing else changes.
 m.hw[9]=7;m.hw[11]=9;
 auto r=retire(m.bytes,10,nullptr,reasonNoContext,true);
 assert(r.outcome==O::Retired && r.returned==0x100 && r.submitted==0x105 && m.hw[10]==0x105);
 assert(m.hw[9]==7 && m.hw[11]==9);
 // Second call: already complete.
 assert(retire(m.bytes,10,nullptr,reasonNoContext,true).outcome==O::NotBusy && m.hw[10]==0x105);
 // Hardware ahead of the bookkeeping is never moved backwards.
 m.hw[10]=0x106;assert(retire(m.bytes,10,nullptr,reasonNoContext,true).outcome==O::NotBusy && m.hw[10]==0x106);
 // Stamp wraparound uses signed distance.
 m.hw[10]=0xFFFFFFF0;m.submitted(10,0x10);
 r=retire(m.bytes,10,nullptr,reasonNoContext,true);
 assert(r.outcome==O::Retired && m.hw[10]==0x10 && r.submitted-r.returned==0x20);
 // Index and table checks.
 m.restart(64);assert(retire(m.bytes,64,nullptr,reasonNoContext,true).outcome==O::Range);
 m.restart(-1);assert(retire(m.bytes,-1,nullptr,reasonNoContext,true).outcome==O::Range);
 m.restart(12);m.addresses[12]=nullptr;m.submitted(12,5);
 assert(retire(m.bytes,12,nullptr,reasonNoContext,true).outcome==O::Range);
 m.put<volatile uint32_t **>(stampAddresses,nullptr);
 assert(retire(m.bytes,12,nullptr,reasonNoContext,true).outcome==O::Range);
 Machine big(300);big.restart(5);big.submitted(5,3);
 assert(retire(big.bytes,5,nullptr,reasonNoContext,true).outcome==O::Range && big.hw[5]==0);
 static_assert(static_cast<unsigned>(O::Retired)+1==outcomes,"one counter per outcome");
 static_assert(recordWords==10,"registry record size");
}
