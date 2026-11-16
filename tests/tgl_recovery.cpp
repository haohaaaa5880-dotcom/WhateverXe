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
	completionCases();
}
