// SPDX-License-Identifier: GPL-2.0-only
/* Actual production fake-callback fixture, no device/IO/syscall interfaces. */
#define main existing_stage_fixture_main
#include "test_ppn_stage.c"
#undef main
int main(void)
{
	struct fake f;
	struct ppn_stage_engine e, saved;
	struct ppn_stage_result first, second;
	unsigned int calls, assertions = 0;

	for (unsigned int fault_mode = 0; fault_mode < 3; fault_mode++) {
		fresh(&f, &e);
		f.wait_fail = 5;
		if (fault_mode == 0)
			f.cancel_fail = 5;
		else if (fault_mode == 1)
			f.cancel_fail = 6;
		else
			f.late_cancel = PPN_STAGE_CLEANUP_TIMEOUT_US;
		assert(program(&e, &first, 1000000) == -ETIMEDOUT);
		assert(first.unquiesced_dma && first.cleanup_error && e.blocked);
		assert(e.data_lease || e.meta_lease);
		saved = e;
		calls = f.calls;
		for (unsigned int erase = 0; erase < 2; erase++) {
			for (unsigned int reuse = 0; reuse < 2; reuse++) {
				struct ppn_stage_result *r = reuse ? &first : &second;
				memset(r, 0xa5, sizeof(*r));
				int ret = erase ? ppn_stage_erase(&e, &address, 1000000, r) : program(&e, r, 1000000);
				assert(ret == -EIO);
				assert(r->durability_unknown && !r->transport_complete);
#ifdef HISTORICAL_CONTROL
				assert(!r->unquiesced_dma);
#else
				assert(r->unquiesced_dma);
#endif
				assert(!r->cleanup_error); /* No cleanup attempt on rejected call. */
				assert(!memcmp(&saved, &e, sizeof(e)) && f.calls == calls);
				assert(e.loaned_data == data && e.loaned_meta == meta);
				assertions += 6;
			}
		}
	}
	/* Quiesced failure/invalid arguments don't invent a pending DMA warning. */
	fresh(&f, &e);
	f.wait_fail = 5;
	assert(program(&e, &first, 1000000) == -ETIMEDOUT);
	idle(&f, &e);
	assert(program(&e, &second, 1000000) == -EIO);
	assert(!second.unquiesced_dma && second.durability_unknown);
	fresh(&f, &e);
	assert(program(&e, &second, 0) == -EINVAL);
	assert(!second.unquiesced_dma && !second.transport_complete && second.durability_unknown);
	printf("PASS %u held-buffer rejection assertions, historical_control=%d\n", assertions + 6,
#ifdef HISTORICAL_CONTROL
		1
#else
		0
#endif
	);
	return 0;
}
