/* C++ EH with a STATICALLY-LINKED thrower — reproduces the real 1990s MSVC case where the CRT
   is linked into the binary, so `_CxxThrowException` lives in .text (not imported). It funnels
   the throw through the imported kernel32 `RaiseException` (a syscall wrapper the CRT cannot
   inline), exactly as the static CRT does. ARET must therefore dispatch the C++ exception from
   inside `aret_RaiseException` (code 0xE06D7363), not via the imported-`_CxxThrowException`
   shim. The catch handler (`__CxxFrameHandler3`) is still imported here, so this isolates the
   thrower-side of static linking. Oracle vs Wine: r = 42 + 7 = 49.

   The local `_CxxThrowException` (a strong definition) overrides the import library's symbol —
   lld-link resolves the object's definition and never pulls the archive member. */
extern "C" __declspec(dllimport) int printf(const char*, ...);
extern "C" __declspec(dllimport) void __stdcall RaiseException(unsigned, unsigned, unsigned, const unsigned*);

extern "C" void __stdcall _CxxThrowException(void* pObject, void* pThrowInfo) {
    unsigned args[3] = { 0x19930520u, (unsigned)(unsigned long)pObject, (unsigned)(unsigned long)pThrowInfo };
    RaiseException(0xE06D7363u, 1u /*NONCONTINUABLE*/, 3u, args);
}

struct E { int code; };

extern "C" int mainCRTStartup() {
    int r = 0;
    try { throw E{42}; } catch (E& e) { r += e.code; }
    try { throw 7;      } catch (int x) { r += x;      }
    printf("r=%d\n", r);   /* Wine: 42 + 7 = 49 */
    return 0;
}
