# Retained DMA warnings survive rejected PPN calls

The isolated engine retains loaned buffers and opaque DMA leases when cancellation fails or exceeds its cleanup budget. Previously, a subsequent program or erase call cleared the caller result before checking blocked state. It returnedEIO with `unquiesced_dma=false` while the same leases/buffers remained held. A caller relying on that documented flag could drop its retention obligation.

The result now derives `unquiesced_dma` from the actual held data/meta leases before any rejected-call return. Cleanup error describes the current call; zero on a rejected call means no cleanup was attempted, not that an earlier transfer stopped. No additional callback, cancellation attempt, release, hardware fence or write activation is introduced.

The regression compiles and executes the entire real engine against its existing fake-DMA fixture. Data/meta cancellation failure and deadline overrun are followed by blocked program and erase calls with both fresh and reused result storage.78 assertions check the retained flag, unchanged engine/buffers and no new callback activity. Successful quiescence and invalid initial timeout do not fabricate retention warnings. ASan/UBSan and-Werror pass. Independent root historical controls at998cfc9 reproduce the bug; the candidate also passes all1,910 existing engine cases. Public tests need no Git history, firmware or device.

Reproduce: `python3 tests/test-experimental-ppn-retention.py`. The existing completion dependency manifest was explicitly refreshed for the changed GPL engine/header. NAND remains read-only in the normal kernel; this component stays unregistered and successful transport still provides no durability claim.
