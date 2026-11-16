#include "../WhateverXe/kern_tgl_context_owners.hpp"
#include "../WhateverXe/kern_tgl_reset_trace.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
	unsigned cases=0;
	// The owner tag equals the reset trace's per-slot context tag.
	alignas(16) uint8_t item[0x20] {},context[0x40] {};
	*reinterpret_cast<void **>(item)=context;
	assert(((TGLResetTrace::slot(item)>>48)&0x7FFF)==TGLContextOwners::contextTag(context));++cases;

	// Ring: 1-based sequence, wraps, keeps tag/pid/name; names are bounded.
	TGLContextOwners::Ring ring;
	alignas(16) static uint8_t contexts[300][0x10];
	for (unsigned i=0;i<300;++i)
		TGLContextOwners::note(ring,contexts[i],int32_t(i),i==299 ? "a-very-long-process-name-exceeding" : (i%2 ? "odd" : "even"));
	assert(ring.next==300);
	for (unsigned s=0;s<TGLContextOwners::capacity;++s) {
		const auto &r=ring.slot[s];
		assert(r.seq && (r.seq-1)%TGLContextOwners::capacity==s && r.seq>300-TGLContextOwners::capacity);
		const unsigned i=unsigned(r.seq-1);
		assert(r.pid==int32_t(i) && r.tag==TGLContextOwners::contextTag(contexts[i]));
		if (i==299) assert(strlen(r.name)==TGLContextOwners::nameLength-1);
		else assert(!strcmp(r.name,i%2 ? "odd" : "even"));
	}
	++cases;
	// A missing name leaves an empty, terminated field.
	TGLContextOwners::note(ring,context,7,nullptr);
	assert(ring.slot[300%TGLContextOwners::capacity].name[0]==0);++cases;
	printf("%u context owner cases passed: trace tag, ring wrap, bounded names\n",cases);
}
