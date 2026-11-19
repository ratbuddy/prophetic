// Stand-in for the Profligacy asmjit dynarec: never compiles, so the core always runs its
// interpreter path. (The plugin's speed comes from our own ahead-of-time translator instead.)
#pragma once

class tms57002_device;

namespace tms57002 {
class Jit
{
public:
	long pooled_runs() const { return 0; }
	long pooled_cmem_runs() const { return 0; }
	bool pooled_resume_pending() const { return false; }
	bool run_begun_frame_pooled(tms57002_device &) { return false; }
};
}
