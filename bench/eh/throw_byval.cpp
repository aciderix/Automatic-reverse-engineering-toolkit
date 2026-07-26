/* C++ catch a multi-word class BY VALUE. The caught parameter is a copy of the thrown
   object; MSVC binds it by memcpy-ing sizeOrOffset bytes (a trivially-copyable type) into the
   catch frame slot. This is the case the naive "copy the first word" binding got wrong (only
   e.a would be set, e.b garbage). ARET now copies the CatchableType's full size. Oracle vs
   Wine: r = a + b = 49. */
extern "C" __declspec(dllimport) int printf(const char*, ...);
struct E { int a; int b; };

extern "C" int mainCRTStartup() {
    int r = 0;
    try { throw E{42, 7}; } catch (E e) { r = e.a + e.b; }   /* by value, 8 bytes -> Wine r=49 */
    printf("r=%d\n", r);
    return 0;
}
