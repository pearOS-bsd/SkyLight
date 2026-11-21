/* rdinfo: asks the real SkyLight for the RealtimeDisplayInfo shared memory (SLSGetRealtimeDisplayInfoShmem: a Mach port of a 0x1408 bytes memory entry that CoreDisplay maps
 * in GetLatestRealtimeDisplayInfoMemory) and dumps it, several times.  Build: clang rdinfo.c -framework CoreGraphics -framework ApplicationServices.  Run in the GUI session of a Mac (CGMainDisplayID first: the connection).
 * Only the structure is kept in the repository (docs), not the dump of an Apple machine. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ApplicationServices/ApplicationServices.h>

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("main display %u\n", CGMainDisplayID());		/* makes the connection to the window server (an app does it before anything else) */
	void *h = dlopen("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_NOW);
	mach_port_t (*get)(void) = h ? dlsym(h, "SLSGetRealtimeDisplayInfoShmem") : NULL;
	mach_port_t port;
	mach_vm_address_t addr = 0;
	kern_return_t kr;

	if (!get) { printf("no SLSGetRealtimeDisplayInfoShmem\n"); return 1; }
	port = get();
	printf("port %u\n", port);
	if (port == 0) return 2;
	kr = mach_vm_map(mach_task_self(), &addr, 0x1408, 0, VM_FLAGS_ANYWHERE, port, 0, 0, VM_PROT_READ, VM_PROT_READ, VM_INHERIT_NONE);
	printf("mach_vm_map kr %d addr %#llx\n", kr, addr);
	if (kr) return 3;
	for (int s = 0; s < 3; s++) {
		const uint8_t *p = (const uint8_t *)(uintptr_t)addr;

		printf("== sample %d\n", s);
		for (int i = 0; i < 0x1408; i += 16) {
			int nz = 0;
			for (int k = 0; k < 16 && i + k < 0x1408; k++) nz |= p[i + k];
			if (!nz) continue;
			printf("%04x:", i);
			for (int k = 0; k < 16 && i + k < 0x1408; k++) printf(" %02x", p[i + k]);
			printf("\n");
		}
		usleep(500000);
	}
	return 0;
}
