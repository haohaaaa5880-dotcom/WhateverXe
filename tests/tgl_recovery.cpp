#include "../WhateverXe/kern_tgl_recovery.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <cstdint>

namespace {
alignas(16) uint8_t channel[0x180],affected[0x180],ring[0x100],context[0xD0];
alignas(16) uint8_t owner[0x1300],scheduler[0x200],otherScheduler[0x200];
alignas(16) uint8_t ringVT[0x200],schedulerVT[0x200];
unsigned resets,resumes,stage;bool nativeOK,replaceScheduler;
bool reset(void *r,void *c) {
	assert(r==ring && c==context && stage++==0);++resets;
	// Native side effects must survive. No caller-synthesized completion.
	TGLRecovery::field<uint32_t>(affected,0x178)=0xABCDEF;
	if (replaceScheduler) TGLRecovery::field<void *>(owner,0x1250)=otherScheduler;
	return nativeOK;
}
void resume(void *s) {
	assert(s==(replaceScheduler ? otherScheduler : scheduler) && stage++==1);++resumes;
}
}
// Cross-engine release: a second engine polling MI_SEMAPHORE_WAIT for skipped work.
namespace rel {
alignas(16) uint8_t sched[0x1000],accel[0x1400],streamer[6][0x80],element[6][0x10],ctx[6][0xC0],chan[6][0x140],rring[6][0x80];
alignas(16) uint8_t vtBlit[0x200],vtMain[0x200],vtMedia[0x200],mmio[1];
uint32_t ipehr,acthd[2],reads;bool moving;unsigned rresets;void *lastRing,*lastCtx;
uint32_t read(void *m,uint32_t off) {
	assert(m==mmio);++reads;
	if (off==0x22068 || off==0x2068) return ipehr;
	if (off==0x22074 || off==0x2074) return moving ? acthd[reads&1]++ : acthd[0];
	assert(false);return 0;
}
void delay(unsigned us) {assert(us==1000);}
bool reset(void *r,void *c) {++rresets;lastRing=r;lastCtx=c;return true;}
void build(unsigned engine,uint8_t *vt) {
	using namespace TGLRecovery;
	field<uint64_t>(accel,0x1300)|=1ULL<<engine;
	field<void *>(sched,0xCA0+8*engine)=streamer[engine];
	field<uint32_t>(streamer[engine],0x50)=0;
	field<void *>(streamer[engine],0x30)=element[engine];
	field<void *>(element[engine],0)=ctx[engine];
	field<void *>(ctx[engine],0xB8)=chan[engine];
	field<void *>(chan[engine],0x130)=rring[engine];
	field<void *>(rring[engine],0)=vt;field<void *>(rring[engine],0x20)=ctx[engine];field<void *>(rring[engine],0x50)=mmio;
}
}
void completionCases() {
	using namespace TGLRecovery;
	alignas(16) static uint8_t em[0x1000];alignas(16) static volatile uint32_t stamps[64];
	static volatile uint32_t *table[64];
	struct Case {const char *name;int32_t count;uint32_t index,hw,issued;bool table;uint32_t expectHW;bool forced;};
	const Case cases[] {
		{"behind: raised to last issued",64,5,100,107,true,107,true},
		{"already complete: untouched",64,5,107,107,true,107,false},
		{"hardware ahead (wrap-safe): untouched",64,5,0x00000002,0xFFFFFFF0,true,0x00000002,false},
		{"wraps forward",64,5,0xFFFFFFF0,0x00000003,true,0x00000003,true},
		{"index out of range",64,64,1,9,true,1,false},
		{"no stamp table",64,5,1,9,false,1,false},
	};
	for (auto &c:cases) {
		memset(em,0,sizeof(em));
		for (unsigned i=0;i<64;++i){stamps[i]=0;table[i]=&stamps[i];}
		field<int32_t>(em,0x30)=c.count;field<volatile uint32_t **>(em,0x28)=c.table ? table : nullptr;
		const unsigned slot=c.index<64 ? c.index : 0;
		stamps[slot]=c.hw;field<uint32_t>(em,0xFC+24*(c.index<64 ? c.index : 0))=c.issued;
		const auto r=completeStamp(em,c.index);
		if (stamps[slot]!=c.expectHW || r.forced!=c.forced) {printf("FAIL %s hw=%x\n",c.name,stamps[slot]);assert(false);}
		// Never beyond the last issued value.
		if (r.forced) assert(stamps[slot]==c.issued);
	}
	assert(!completeStamp(nullptr,0).forced);
	puts("7 completion cases passed: raise to last issued only, never past it; complete, ahead, range, table and null untouched");
}
void releaseCases() {
	using namespace TGLRecovery;using namespace rel;
	for (auto vt:{vtBlit,vtMain,vtMedia}) field<decltype(&rel::reset)>(vt,0x168)=rel::reset;
	const Probe probe {{{reinterpret_cast<uintptr_t>(vtBlit),0x22000},{reinterpret_cast<uintptr_t>(vtMain),0x2000}},read,delay};
	struct Case {const char *name;uint32_t header;bool move,inflight,known,attributed;unsigned expect;};
	const Case cases[] {
		{"stalled blitter semaphore wait",0x0E009002,false,true,true,false,1},
		{"blitter semaphore wait still advancing",0x0E009002,true,true,true,false,0},
		{"blitter in another command",0x7A000004,false,true,true,false,0},
		{"blitter without in-flight element",0x0E009002,false,false,true,false,0},
		{"unknown rring class",0x0E009002,false,true,false,false,0},
		{"the attributed ring itself",0x0E009002,false,true,true,true,0},
	};
	for (auto &c:cases) {
		memset(sched,0,sizeof(sched));memset(accel,0,sizeof(accel));memset(streamer,0,sizeof(streamer));
		field<void *>(sched,0x10)=accel;
		build(0,vtMain); // attributed render engine
		build(3,c.known ? vtBlit : vtMedia);
		if (!c.inflight) field<void *>(streamer[3],0x30)=nullptr;
		ipehr=c.header;moving=c.move;acthd[0]=0x100;acthd[1]=0x200;reads=rresets=0;lastRing=lastCtx=nullptr;
				void *attributed=c.attributed ? static_cast<void *>(rring[3]) : static_cast<void *>(rring[0]);
		field<void *>(streamer[0],0x30)=nullptr; // render engine idle: only the blitter is a candidate
		const auto r=releaseWaiters(sched,attributed,probe);
		if (rresets!=c.expect || r.resets!=c.expect || r.resetOK!=c.expect) {printf("FAIL %s rresets=%u\n",c.name,rresets);assert(false);}
		if (c.expect) assert(lastRing==rring[3] && lastCtx==ctx[3] && r.stalled==1 && r.semaphore==1);
	}
	// No probe: native single-pass behaviour unchanged (covered above); empty engine mask: nothing read.
	memset(accel,0,sizeof(accel));field<void *>(sched,0x10)=accel;reads=0;
	assert(releaseWaiters(sched,nullptr,probe).busy==0 && reads==0);
	puts("7 release cases passed: stalled semaphore waiter reset once; advancing, other command, idle, unknown class, attributed ring and empty mask untouched");
}
int main() {
	for (bool ok:{false,true}) for (bool replace:{false,true}) for (unsigned initial:{0U,5U,0xFFFFFFFFU}) {
		memset(channel,0,sizeof(channel));memset(affected,0,sizeof(affected));
		TGLRecovery::field<void *>(channel,0x130)=ring;
		TGLRecovery::field<void *>(channel,0x18)=owner;
		TGLRecovery::field<void *>(ring,0)=ringVT;
		TGLRecovery::field<void *>(ring,0x20)=context;
		TGLRecovery::field<void *>(context,0xB8)=affected;
		TGLRecovery::field<void *>(owner,0x1250)=scheduler;
		TGLRecovery::field<void *>(scheduler,0)=schedulerVT;
		TGLRecovery::field<void *>(otherScheduler,0)=schedulerVT;
		TGLRecovery::field<decltype(&reset)>(ringVT,0x168)=reset;
		TGLRecovery::field<decltype(&resume)>(schedulerVT,0x180)=resume;
		TGLRecovery::field<uint32_t>(affected,0x20)=2;
		TGLRecovery::field<uint32_t>(affected,0x17C)=initial;
		nativeOK=ok;replaceScheduler=replace;resets=resumes=stage=0;
		const auto result=TGLRecovery::resetAndResume(channel);
		assert(resets==1 && resumes==1 && stage==2 && result.resetOK==ok);
		assert(result.affected==reinterpret_cast<uintptr_t>(affected) && result.index==2);
		assert(result.failuresBefore==initial && result.failuresAfter==initial+unsigned(!ok));
		assert(TGLRecovery::field<uint32_t>(affected,0x178)==0xABCDEF);
		// Affected channel comes from the context; do not update caller blindly.
		assert(TGLRecovery::field<uint32_t>(channel,0x17C)==0);
	}
	puts("12 recovery cases passed: native result, count, order, affected channel, owner reload");
	releaseCases();
	completionCases();
}
