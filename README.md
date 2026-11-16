# SkyLight — our SkyLight

The window server of the Mac speaks the SkyLight protocol (SLS / CGS: MIG messages on the `com.apple.windowserver.active` port).  This repository is our implementation of it, from the decompile and from recordings of the real WindowServer.

| folder | what |
|---|---|
| `include/` | `sls_msgids.h` (the message ids), `sls_wire.h` (the structures of the wire), `SkyLight.h` (the client API: SLS/CGS functions as the shim provides them) |
| `server/` | the handlers that the window server links: `sls_window.c`, `sls_session.c`, `sls_space.c`, `sls_cursor.c`, `sls_tap.c` (event taps), `sls_transaction.c`, `sls_hotkey*.c`, `sls_display*.c`, `sls_carender.c`, `sls_ext.c`, and the generated ones (`sls_golden.c`, `sls_generic.c`, `sls_blobs.c`, `sls_harvest.c`, `sls_zero_ndr.h`) |
| `client/` | `skylight.c`: the SLS/CGS C API over the wire client of the WindowServer repository (`ws_client.h`) |
| `tools/` | the generators of the generated files (`gen_golden.py`, `gen_generic.py`, `gen_blobs.py`, `gen_hotkeys.py`, `gen_harvest.py`, `gen_zero_ndr.py`, `generic_remaining.py`) |
| `docs/` | `SPEC-SLS-WIRE.md` (the wire), `EVENT-TAPS.md`, the message/opcode tables of SkyLight 26.7.2 |

Used by `pearOS-bsd/WindowServer` (`make SKYLIGHT=path/of/SkyLight`; the default is `../SkyLight`): its Makefile compiles `server/*.c` into wsd and `client/skylight.c` into the shim, with `-I<SkyLight>/include`.
The recordings the generators read (`docs/golden`) and the tests that exercise the shim (`test_shim*`, `test_wire`, `test_golden`) stay with the WindowServer.
