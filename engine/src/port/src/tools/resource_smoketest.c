// resource_smoketest.c - proves game_resource.c's Resource_Acquire/Release engine
// against a functional fake descriptor, deterministically and with no game deps.
//
// Verifies the exact resource.h contract the real engine relies on:
//  - Resource_Acquire returns &gResourceLoadedHandles[id], whose target is the
//    descriptor's data (methods) at offset 0x18 - so (*handle)[k] is method k.
//  - acquire() runs once (refcount 0->1), not again on re-acquire.
//  - refcount increments per acquire; Release decrements and calls release() at 0.
//  - unbrought-up ids return the no-op stub (a callable method, never NULL).

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RDC 0x2c1
typedef struct RD {
    unsigned int metadata[4];
    void (*acquire)(struct RD* d);
    void (*release)(void);
} RD;

extern RD*   gResourceDescriptors[RDC];
extern void* gResourceLoadedHandles[RDC];
extern unsigned short gResourceRefCounts[RDC];
extern void* Resource_Acquire(unsigned short id, int unused);
extern int   Resource_Release(void* handleSlot);
extern void  Resource_ResetRefCounts(void);

// A functional fake DLL descriptor: header + 3 method pointers at offset 0x18.
static int gAcquireCalls, gReleaseCalls, gMethod0Calls;
static void fakeAcquire(RD* d) { (void)d; gAcquireCalls++; }
static void fakeRelease(void)  { gReleaseCalls++; }
static void fakeMethod0(void)  { gMethod0Calls++; }
static struct FakeDesc {
    unsigned int metadata[4];
    void (*acquire)(RD*);
    void (*release)(void);
    void* methods[3];
} gFake;

#define TEST_ID 200

static int gFails;
static void chk(const char* what, long got, long want) {
    if (got != want) { printf("  FAIL %-26s got=%ld want=%ld\n", what, got, want); gFails++; }
    else               printf("  ok   %-26s = %ld\n", what, got);
}

int main(void) {
    // Trigger registry init (all stub descriptors) via one acquire.
    void* h0 = Resource_Acquire(1, 0);
    printf("stub id: acquire returns non-null handle, method callable\n");
    chk("stub handle non-null", h0 != 0, 1);
    void** stubTbl = (void**)(*(void**)h0);
    chk("stub method non-null", stubTbl[0] != 0, 1);   // no-op, never NULL
    ((void(*)(void))stubTbl[0])();                       // must not crash

    // Install a functional fake descriptor at TEST_ID and reset state.
    memset(&gFake, 0, sizeof gFake);
    gFake.acquire = fakeAcquire;
    gFake.release = fakeRelease;
    gFake.methods[0] = (void*)fakeMethod0;
    gResourceDescriptors[TEST_ID] = (RD*)&gFake;
    gResourceRefCounts[TEST_ID] = 0;
    gAcquireCalls = gReleaseCalls = gMethod0Calls = 0;

    printf("real descriptor at id %d:\n", TEST_ID);
    void* h = Resource_Acquire(TEST_ID, 0);
    chk("acquire() called once", gAcquireCalls, 1);
    chk("refcount", gResourceRefCounts[TEST_ID], 1);

    // handle target must be &descriptor->data (offset 0x18) = &gFake.methods
    void** methods = (void**)(*(void**)h);
    chk("handle -> data offset 0x18", (long)((char*)methods - (char*)&gFake), 0x18);
    chk("method0 identity", methods[0] == (void*)fakeMethod0, 1);
    ((void(*)(void))methods[0])();
    chk("method0 invoked", gMethod0Calls, 1);

    void* h2 = Resource_Acquire(TEST_ID, 0);
    chk("re-acquire same handle", h2 == h, 1);
    chk("acquire() not re-called", gAcquireCalls, 1);
    chk("refcount now 2", gResourceRefCounts[TEST_ID], 2);

    chk("release: refcount>0 -> 0 ret", Resource_Release(h), 0);
    chk("release() not yet called", gReleaseCalls, 0);
    chk("release: last ref -> 1 ret", Resource_Release(h), 1);
    chk("release() called at 0", gReleaseCalls, 1);
    chk("refcount back to 0", gResourceRefCounts[TEST_ID], 0);

    printf("\n%s (%d failure%s)\n", gFails ? "SMOKETEST FAILED" : "SMOKETEST PASSED",
           gFails, gFails == 1 ? "" : "s");
    return gFails ? 1 : 0;
}
