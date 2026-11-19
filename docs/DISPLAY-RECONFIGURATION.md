# Display reconfiguration: what the clients of the real WindowServer expect (macOS 26.7.2 SkyLight, decompiled 2026-11-20)

Evidence: `SkyLight` of the dyld cache (`llvm-objdump` of `initDisplays`, `___initDisplays_block_invoke`, `_displayConfigNotifyProc`, `_CGSReconfigNotifierCalloutListInit`).

* The process-wide display code of SkyLight (`initDisplays`, run once per process by `SLDisplayRegisterReconfigurationCallback` and the other display calls) adds six callbacks to the process notifier (`ProcessNotifier::add_callback(type, proc, ...)`), i.e. the process asks the server for these notification types:
  | type | callback | meaning |
  |---|---|---|
  | `0x64` (100) | `displayWillConfigNotifyProc` | a display is about to be reconfigured |
  | `0x70` (112) / `0x71` (113) | `displayConfigAddMonitorProc` / `displayConfigRemoveMonitorProc` | a display was added / removed |
  | `0x72` (114) | `displayConfigNotifyProc` | the configuration changed |
  | `0x73` (115) | `displayConfigFinalizedProc` | the reconfiguration is finalized: the process calls its `CGDisplayReconfigurationCallBack`s (the list is copied by `CGSReconfigNotifierCalloutListInit`) |
  | `0x79` (121) | `displayAcceleratorChangedProc` | the display's GPU changed |
* The callbacks do not read the new geometry from the notification: they query the server (`SLSDisplayStatusQuery`, the display list, bounds and modes), update their `gDisplayList`, flag what changed and call the registered callbacks.  So the server has to (1) answer the queries with the new size and (2) send 100, then 114, then 115 to every connection whose interests list those types.
* AppKit turns the callback into `NSApplicationDidChangeScreenParametersNotification`, which is what the Finder's desktop window, the Dock and the menu bar act on.

Implementation: `WindowServer/src/wsd.c`, `display_changed()` pushes the three notifications (as a datagram of kind 1, `{type, 0, 0}`, the form of `WSD_TEST_DG=2`) to the connections of Mach clients that asked for them; until this is verified on a Mac client the Dock, SystemUIServer and Finder are also restarted as before (`WSD_NO_RESTART_CLIENTS=1` tests the real path alone).  Not verified yet: the payload of the notifications, the answers of `SLSDisplayStatusQuery` after a resize.
