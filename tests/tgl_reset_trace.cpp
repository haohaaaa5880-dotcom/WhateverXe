#include "../WhateverXe/kern_tgl_reset_trace.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

namespace {
alignas(16) uint8_t streamer[0x1C10],descriptor[0x80],items[4][0x20],contexts[4][0x40];
TGLResetTrace::Ring ring;
template<typename T> T &at(void *p,size_t o) { return *reinterpret_cast<T *>(static_cast<uint8_t *>(p)+o); }

void setup(uint8_t engine,uint32_t port,uint32_t head) {
	memset(streamer,0,sizeof(streamer));memset(descriptor,0,sizeof(descriptor));
	descriptor[0]=engine;
	at<void *>(streamer,0x28)=descriptor;
	at<uint32_t>(streamer,0x50)=port;
	at<uint32_t>(streamer,0x1C04)=head;
	for (unsigned i=0;i<4;++i) {
		memset(items[i],0,sizeof(items[i]));
		at<void *>(items[i],0)=contexts[i];
		at<uint32_t>(items[i],0x18)=0x24000+i;
		at<uint32_t>(items[i],0x1C)=0x10000|(i+3); // only the low 16 flag bits are kept
	}
}
}

int main() {
	unsigned cases=0;
	// Slot packing and per-field layout.
	setup(0,1,11);
	at<void *>(streamer,0x30)=items[0];at<void *>(streamer,0x48)=items[3];
	TGLResetTrace::record(ring,123,TGLResetTrace::CSBBefore,streamer,0x00050402,0xDEADBEEFCAFEull);
	const uint64_t *q=ring.record[0];
	assert(q[0]==1 && q[1]==123 && q[3]==0xDEADBEEFCAFEull);
	assert((q[2]&0xFF)==TGLResetTrace::CSBBefore && ((q[2]>>8)&0xFF)==0 && ((q[2]>>16)&0xFF)==1 && ((q[2]>>24)&0xFF)==11);
	assert((q[2]>>32)==0x00050402);
	assert(q[4]>>63 && (q[4]&0xFFFFFFFF)==0x24000 && ((q[4]>>32)&0xFFFF)==3);
	assert(((q[4]>>48)&0x7FFF)==((reinterpret_cast<uintptr_t>(contexts[0])>>4)&0x7FFF));
	assert(q[5]==0 && q[6]==0 && q[7]>>63 && (q[7]&0xFFFFFFFF)==0x24003);
	++cases;
	// Only the render reset domain (RCS 0, CCS 1) is traced; malformed streamers are ignored.
	for (uint8_t engine=0;engine<6;++engine) {
		const uint64_t before=ring.next;setup(engine,0,0);
		TGLResetTrace::record(ring,1,TGLResetTrace::SubmitBefore,streamer,0,0);
		assert(ring.next==before+(engine<=1));++cases;
	}
	{
		const uint64_t before=ring.next;
		TGLResetTrace::record(ring,1,TGLResetTrace::ResetBefore,nullptr,0,0);
		setup(0,0,0);at<void *>(streamer,0x28)=nullptr;
		TGLResetTrace::record(ring,1,TGLResetTrace::ResetBefore,streamer,0,0);
		assert(ring.next==before);++cases;
	}
	// Ring wraps: the newest record overwrites the oldest slot with its 1-based sequence.
	setup(1,0,5);
	while (ring.next<TGLResetTrace::capacity*2+7)
		TGLResetTrace::record(ring,ring.next,TGLResetTrace::ResetAfter,streamer,0,ring.next);
	for (unsigned i=0;i<TGLResetTrace::capacity;++i) {
		const uint64_t *r=ring.record[i];
		assert(r[0] && (r[0]-1)%TGLResetTrace::capacity==i && r[3]==r[0]-1 && r[1]==r[0]-1);
		assert(r[0]>ring.next-TGLResetTrace::capacity && r[0]<=ring.next);
		assert(((r[2]>>8)&0xFF)==1);
	}
	++cases;
	printf("%u reset trace cases passed: slot layout, render-domain filter, ring wrap\n",cases);
}
