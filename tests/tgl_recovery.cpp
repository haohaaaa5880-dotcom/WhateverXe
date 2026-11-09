#include "../WhateverXe/kern_tgl_recovery.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
alignas(16) uint8_t channel[0x180],affected[0x180],ring[0x900],context[0xD0];
alignas(16) uint8_t owner[0x1300],scheduler[0xD00],otherScheduler[0xD00];
alignas(16) uint8_t ringVT[0x200],schedulerVT[0x200],engineRecord[0x100],accelerator[0x10];
alignas(16) uint8_t siblingStreamer[0x80],primaryStreamer[0x80],events[0x10],eventsVT[0x200];
alignas(16) uint8_t workItem[0x20],otherItem[0x20],siblingContext[0xC0],siblingChannel[0x40];
struct Call { int kind; uintptr_t arg; unsigned stage; };
Call calls[8];unsigned ncalls;
unsigned resets,resumes,stage,wakes,siblings,siblingTypeSeen;bool nativeOK,replaceScheduler;
int wakeDepth;
void *listDuringReset,*expectedList;
TGLRecovery::Domain domain;

bool reset(void *r,void *c) {
	assert(r==ring && c==context && stage++==0);++resets;
	listDuringReset=TGLRecovery::field<void *>(ring,0x78);
	// Native side effects must survive. No caller-synthesized completion.
	TGLRecovery::field<uint32_t>(affected,0x178)=0xABCDEF;
	if (replaceScheduler) TGLRecovery::field<void *>(owner,0x1250)=otherScheduler;
	return nativeOK;
}
void resume(void *s) {
	assert(s==(replaceScheduler ? otherScheduler : scheduler));
	assert(wakeDepth==0);++stage;++resumes;
}
void processReset(void *s,uint32_t type,void *ctx) {
	// After reset, before resume, inside a forcewake hold, on the reloaded scheduler.
	assert(s==(replaceScheduler ? otherScheduler : scheduler) && stage==1 && wakeDepth==1 && !ctx);
	assert(TGLRecovery::field<void *>(ring,0x78)==expectedList);
	++siblings;siblingTypeSeen=type;
}
void increment(void *e,uint32_t index) {
	assert(e==events && ncalls<8);calls[ncalls++]={1,index,stage};
}
void submitStamp(void *ch) {
	assert(ncalls<8);calls[ncalls++]={2,reinterpret_cast<uintptr_t>(ch),stage};
}
void forceWake(void *a,bool on,unsigned domainMask) {
	assert(a==accelerator && domainMask==7);
	wakeDepth+=on ? 1 : -1;assert(wakeDepth==0 || wakeDepth==1);++wakes;
}

void setup(bool ok,bool replace,unsigned initial) {
	memset(channel,0,sizeof(channel));memset(affected,0,sizeof(affected));memset(ring,0,sizeof(ring));
	memset(scheduler,0,sizeof(scheduler));memset(otherScheduler,0,sizeof(otherScheduler));
	memset(engineRecord,0,sizeof(engineRecord));
	memset(siblingStreamer,0,sizeof(siblingStreamer));memset(primaryStreamer,0,sizeof(primaryStreamer));
	memset(owner,0,sizeof(owner));ncalls=0;
	TGLRecovery::field<void *>(channel,0x130)=ring;
	TGLRecovery::field<void *>(channel,0x18)=owner;
	TGLRecovery::field<void *>(ring,0)=ringVT;
	TGLRecovery::field<void *>(ring,0x10)=accelerator;
	TGLRecovery::field<void *>(ring,0x20)=context;
	TGLRecovery::field<void *>(context,0xB8)=affected;
	TGLRecovery::field<void *>(owner,0x1250)=scheduler;
	TGLRecovery::field<void *>(scheduler,0)=schedulerVT;
	TGLRecovery::field<void *>(otherScheduler,0)=schedulerVT;
	TGLRecovery::field<decltype(&reset)>(ringVT,0x168)=reset;
	TGLRecovery::field<decltype(&resume)>(schedulerVT,0x180)=resume;
	TGLRecovery::field<decltype(&processReset)>(schedulerVT,0x190)=processReset;
	TGLRecovery::field<uint32_t>(affected,0x20)=2;
	TGLRecovery::field<uint32_t>(affected,0x17C)=initial;
	nativeOK=ok;replaceScheduler=replace;resets=resumes=stage=wakes=siblings=0;
	siblingTypeSeen=0xFF;wakeDepth=0;listDuringReset=nullptr;
}

// Native-shaped lists: RCS (subset of the donor's 24) and CCS (donor's 4).
TGLRecovery::ResetEntry rcsData[] {
	{0x2080,0,0,"HWS_PGA"},{0x20A8,0,0,"CONTROL_IMR"},{0x229C,0,1,""},{0x7000,0,1,""},{0x24D0,0,0,"NP0"}
};
TGLRecovery::ResetEntry ccsData[] {
	{0x1A080,0,0,""},{0x1A0C0,0,1,""},{0x1A09C,0,1,""},{0x1A29C,0,1,""}
};
TGLRecovery::ResetList rcsList {5,5,rcsData}, ccsList {4,4,ccsData}, nativeRing {1,1,rcsData};

void buildCases() {
	// Merge order: RCS, CCS, then CCS extras; masked flags and names preserved.
	TGLRecovery::Domain d;d.native[0]=&rcsList;d.native[1]=&ccsList;
	assert(TGLRecovery::build(d) && d.ready && d.merged.data==d.storage);
	const unsigned extras=sizeof(TGLRecovery::computeExtras)/sizeof(TGLRecovery::computeExtras[0]);
	const unsigned domainExtras=sizeof(TGLRecovery::domainExtras)/sizeof(TGLRecovery::domainExtras[0]);
	assert(d.merged.count==5+domainExtras+4+extras && d.merged.capacity==d.merged.count);
	assert(d.storage[0].reg==0x2080 && d.storage[2].masked==1 && !strcmp(d.storage[1].name,"CONTROL_IMR"));
	auto at=[&](uint32_t reg)->int {
		for (uint64_t i=0;i<d.merged.count;++i) if (d.storage[i].reg==reg) return int(i);
		return -1;
	};
	// RCU_MODE (CCS enable, masked) follows the RCS list and precedes every CCS register.
	assert(at(0x14800)==5 && d.storage[5].masked==1);
	for (uint32_t reg:{0x1A080U,0x1A0C0U,0x1A09CU,0x1A29CU,0x1A0A8U,0x1A058U,0x1A4D0U,0x1A4D4U,0x1A0D4U})
		assert(at(reg)>at(0x14800));
	assert(at(0x1A29C)>=0 && d.storage[at(0x1A29C)].masked==1);
	for (uint32_t reg:{0x24D8U,0x24DCU,0x24E0U,0x24F0U,0x24F4U,0x24F8U,0x24FCU})
		assert(at(reg)>0 && !d.storage[at(reg)].masked);
	// RING_PSMI_CTL wait-for-event workaround, masked, on both render-domain engines;
	// the CCS copy follows the RCU_MODE CCS enable.
	assert(at(0x2050)>0 && d.storage[at(0x2050)].masked==1);
	assert(at(0x20B4)>0 && !d.storage[at(0x20B4)].masked); // RCS EMR
	assert(at(0x1A050)>at(0x14800) && d.storage[at(0x1A050)].masked==1);
	bool imr=false;
	for (uint64_t i=0;i<d.merged.count;++i) {
		assert(d.storage[i].value==0); // values come only from the native pre-reset sample
		for (uint64_t j=i+1;j<d.merged.count;++j) assert(d.storage[i].reg!=d.storage[j].reg);
		if (d.storage[i].reg==0x1A0A8) imr=!d.storage[i].masked;
	}
	assert(imr);
	// Duplicate registers across lists collapse; a native extra is not duplicated.
	TGLRecovery::ResetEntry dupData[] {{0x1A080,0,0,""},{0x1A0A8,0,0,"IMR"}};
	TGLRecovery::ResetList dup {2,2,dupData};
	d.native[1]=&dup;assert(TGLRecovery::build(d) && d.merged.count==5+domainExtras+2+extras-1);
	// Fail closed on every unexpected layout.
	TGLRecovery::ResetEntry bad[] {{0x2081,0,0,""}};
	TGLRecovery::ResetList badReg {1,1,bad}, empty {0,0,rcsData}, nullData {1,1,nullptr}, huge {33,33,rcsData};
	for (auto *l:{&badReg,&empty,&nullData,&huge}) {
		d.native[1]=l;assert(!TGLRecovery::build(d) && !d.ready);
	}
	d.native[1]=nullptr;assert(!TGLRecovery::build(d) && !d.ready);
	puts("build: merge, dedupe, IMR extra, fail-closed layouts passed");
}
}

static unsigned reinitCalls;static bool reinitOrderOK,reinitAnswer;static void *reinitOwner;
static bool reinit(void *o) {
	++reinitCalls;reinitOwner=o;
	if (resets!=1 || siblings!=0 || resumes!=0) reinitOrderOK=false;
	return reinitAnswer;
}
int main() {
	// Original contract, no domain: identical native single-ring behavior.
	unsigned cases=0;
	for (bool ok:{false,true}) for (bool replace:{false,true}) for (unsigned initial:{0U,5U,0xFFFFFFFFU}) {
		setup(ok,replace,initial);
		const auto result=TGLRecovery::resetAndResume(channel);
		assert(resets==1 && resumes==1 && stage==2 && result.resetOK==ok && !siblings && !wakes);
		assert(result.affected==reinterpret_cast<uintptr_t>(affected) && result.index==2);
		assert(result.failuresBefore==initial && result.failuresAfter==initial+unsigned(!ok));
		assert(TGLRecovery::field<uint32_t>(affected,0x178)==0xABCDEF);
		// Affected channel comes from the context; do not update caller blindly.
		assert(TGLRecovery::field<uint32_t>(channel,0x17C)==0);
		assert(!result.domainList && !result.siblingReset && result.type==0xFF);
		++cases;
	}
	buildCases();
	domain={};domain.native[0]=&rcsList;domain.native[1]=&ccsList;assert(TGLRecovery::build(domain));

	// Domain cases: type x reset result x scheduler reload x engine flags x sibling presence.
	for (uint8_t type:{0,1,2}) for (bool ok:{false,true}) for (bool replace:{false,true})
	for (uint32_t flags:{0U,4U,8U}) for (bool present:{false,true}) for (uint32_t mask:{2U,4U}) {
		setup(ok,replace,0);
		TGLRecovery::field<uint8_t>(ring,0x40)=type;
		TGLRecovery::field<uint32_t>(ring,0x70)=mask;
		TGLRecovery::field<void *>(ring,0x78)=&nativeRing;
		TGLRecovery::field<void *>(ring,0x8B8+type*8u)=engineRecord;
		TGLRecovery::field<uint32_t>(engineRecord,0xA4)=flags;
		const uint8_t sibling=type==0 ? 1 : 0;
		void *s=replace ? otherScheduler : scheduler;
		if (present) TGLRecovery::field<void *>(s,0xCA0+sibling*8u)=siblingStreamer;
		expectedList=&nativeRing;
		const bool shared=mask==2 && type<2 && !flags;
		reinitCalls=0;reinitOrderOK=true;reinitOwner=nullptr;reinitAnswer=!present;
		const auto result=TGLRecovery::resetAndResume(channel,&domain,forceWake,nullptr,reinit);
		assert(resets==1 && resumes==1 && wakeDepth==0);
		// Native cache/L3 re-init: only after a successful shared GDRST, with the
		// engines idle (after reset, before sibling replay and scheduler resume).
		assert(reinitCalls==unsigned(shared && ok) && reinitOrderOK);
		if (reinitCalls) assert(reinitOwner==owner && result.cacheReinit==reinitAnswer);
		else assert(!result.cacheReinit);
		// The merged list is visible ONLY during native reset, then restored.
		assert(listDuringReset==(shared ? static_cast<void *>(&domain.merged) : static_cast<void *>(&nativeRing)));
		assert(TGLRecovery::field<void *>(ring,0x78)==&nativeRing);
		assert(result.domainList==shared && result.failuresAfter==unsigned(!ok));
		const bool expectSibling=shared && ok && present;
		assert(result.siblingReset==expectSibling && siblings==unsigned(expectSibling));
		assert(wakes==(expectSibling ? 2U : 0U));
		if (expectSibling) assert(siblingTypeSeen==sibling && result.siblingType==sibling);
		if (shared) assert(result.type==type); else assert(result.type==0xFF);
		++cases;
	}
	// Not ready / no forcewake: native behavior.
	for (int variant=0;variant<2;++variant) {
		setup(true,false,0);
		TGLRecovery::field<uint32_t>(ring,0x70)=2;
		TGLRecovery::field<void *>(ring,0x78)=&nativeRing;
		TGLRecovery::field<void *>(ring,0x8B8)=engineRecord;
		TGLRecovery::field<void *>(scheduler,0xCA0+8)=siblingStreamer;
		TGLRecovery::Domain idle;idle.native[0]=&rcsList;idle.native[1]=&ccsList;
		if (variant) assert(TGLRecovery::build(idle));
		const auto result=TGLRecovery::resetAndResume(channel,&idle,variant ? nullptr : forceWake);
		assert(listDuringReset==&nativeRing && !siblings && !result.domainList);
		++cases;
	}
	// Completion stamps (native resetHardwareAndReplay 4CA79, without sleepForStamp).
	TGLRecovery::field<void *>(events,0)=eventsVT;
	TGLRecovery::field<decltype(&increment)>(eventsVT,0x1D8)=increment;
	for (int scenario=0;scenario<7;++scenario) for (uint32_t port:{0U,1U}) {
		setup(scenario!=5,false,0);
		const bool domainOn=scenario!=0;
		TGLRecovery::field<void *>(owner,0x380)=scenario==4 ? nullptr : static_cast<void *>(events);
		TGLRecovery::field<uint8_t>(ring,0x40)=0;
		TGLRecovery::field<uint32_t>(ring,0x70)=2;
		TGLRecovery::field<void *>(ring,0x78)=&nativeRing;
		TGLRecovery::field<void *>(ring,0x8B8)=engineRecord;
		TGLRecovery::field<void *>(scheduler,0xCA0)=primaryStreamer;
		TGLRecovery::field<void *>(scheduler,0xCA8)=siblingStreamer;
		memset(workItem,0,sizeof(workItem));memset(otherItem,0,sizeof(otherItem));
		TGLRecovery::field<void *>(workItem,0)=siblingContext;
		TGLRecovery::field<uint32_t>(workItem,0x18)=0x77;TGLRecovery::field<uint32_t>(workItem,0x1C)=2;
		TGLRecovery::field<void *>(siblingContext,0xB8)=scenario==2 ? static_cast<void *>(affected) : static_cast<void *>(siblingChannel);
		TGLRecovery::field<uint32_t>(siblingChannel,0x20)=31;
		TGLRecovery::field<uint32_t>(siblingStreamer,0x50)=scenario==3 ? 5 : port;
		TGLRecovery::field<void *>(siblingStreamer,0x30+port*0x10)=workItem;
		TGLRecovery::field<void *>(siblingStreamer,0x30+(1-port)*0x10+8)=otherItem;
		TGLRecovery::field<uint32_t>(otherItem,0x18)=0x99;
		if (scenario==6) TGLRecovery::field<void *>(siblingStreamer,0x30+port*0x10)=nullptr;
		expectedList=&nativeRing;
		const auto result=TGLRecovery::resetAndResume(channel,domainOn ? &domain : nullptr,
			domainOn ? forceWake : nullptr,submitStamp);
		assert(resets==1 && resumes==1);
		const bool siblingStamp=domainOn && scenario!=2 && scenario!=3 && scenario!=4 && scenario!=5 && scenario!=6;
		const unsigned expectCalls=scenario==4 ? 0 : (siblingStamp ? 4 : 2);
		assert(ncalls==expectCalls && result.stampsAppended==expectCalls/2);
		for (unsigned i=0;i<ncalls;++i) assert(calls[i].stage==2); // strictly after resume
		if (ncalls) {
			assert(calls[0].kind==1 && calls[0].arg==2);
			assert(calls[1].kind==2 && calls[1].arg==reinterpret_cast<uintptr_t>(affected));
		}
		if (siblingStamp) {
			assert(calls[2].kind==1 && calls[2].arg==31);
			assert(calls[3].kind==2 && calls[3].arg==reinterpret_cast<uintptr_t>(siblingChannel));
		}
		// The sibling channel is identified (and recorded) whenever the sibling
		// reset runs, even when no stamp can be appended (no event machine).
		const bool siblingSeen=domainOn && (scenario==1 || scenario==4);
		assert(result.siblingIndex==(siblingSeen ? 31U : 0xFFFFFFFFU));
		// Pre-reset snapshot of the sibling (CCS) streamer.
		const auto &s=result.ports[1];
		assert(s.port==(scenario==3 ? 5U : port));
		if (scenario!=6) assert(s.present[port][0] && s.stamp[port][0]==0x77 && s.flags[port][0]==2);
		assert(s.present[1-port][1] && s.stamp[1-port][1]==0x99 && !s.present[port][1]);
		assert(result.ports[0].port==0 && !result.ports[0].present[0][0]);
		++cases;
	}
	printf("%u recovery cases passed: native contract, render-domain list swap, sibling replay, completion stamps\n",cases);
}
