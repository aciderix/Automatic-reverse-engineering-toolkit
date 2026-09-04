/* LEVIER 1 on the GNU C++ RUNTIME — C++11 std::exception_ptr transport. Regression guard
 * for the bug where std::current_exception() returned a NULL exception_ptr (captured=0 vs
 * Wine's captured=1), then std::rethrow_exception(null) crashed: a SILENTLY WRONG result
 * (§0). Root cause: the reference count that keeps a thrown object alive while an
 * exception_ptr references it lives in a __cxa_exception HEADER before the object (mingw
 * libstdc++: object at malloc-base+0x60, referenceCount at object-0x60, exceptionDestructor
 * at object-0x4c), which ARET's closed model never allocated — so the lifted
 * exception_ptr::_M_addref/_M_release wrote below the allocation and current_exception read
 * an empty globals->caughtExceptions. The fix reserves that header at __cxa_allocate_exception
 * and host-backs std::current_exception / std::rethrow_exception (they need __cxa_get_globals /
 * a __cxa_dependent_exception the closed model doesn't build) to drive the same refcount.
 *
 * Coverage: (1) capture in a catch(...) and carry the exception OUT of the handler (the
 * refcount must survive __cxa_end_catch); (2) rethrow it later and catch by base — .what()
 * back into lifted libstdc++; (3) a captured exception that is NEVER rethrown must still be
 * destroyed exactly once when the last exception_ptr dies (refcount to 0 -> dtor + free).
 * __cxa_* / the exception_ptr transport are ARET HLE; libstdc++/libgcc/libwinpthread are
 * LIFTED (.withlocaldll) and the SAME files load beside the exe under Wine => bit-identical. */
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>

/* A type whose destructor is observable, so a leaked or double-freed exception object shows
 * up as a wrong ~Payload count in the diffed output. */
struct Payload : std::runtime_error {
    Payload(const std::string& m) : std::runtime_error(m) { printf("Payload(%s)\n", what()); }
    Payload(const Payload& o) : std::runtime_error(o) { printf("Payload-copy\n"); }
    ~Payload() { printf("~Payload\n"); }
};

static std::exception_ptr capture(const char* msg) {
    try { throw Payload(msg); }                 /* thrown from lifted libstdc++ (std::string ctor) */
    catch (...) { return std::current_exception(); }   /* carried OUT of the handler */
    return nullptr;
}

int main() {
    setvbuf(stdout, 0, _IONBF, 0);
    printf("start\n");

    /* (1)+(2): capture, carry past the catch, rethrow later, catch by base. */
    std::exception_ptr p = capture("carried");
    printf("captured=%d\n", p ? 1 : 0);
    try {
        std::rethrow_exception(p);
    } catch (const std::exception& e) {
        printf("rethrown: %s\n", e.what());
    }

    /* (3): a captured exception that is never rethrown — destroyed once when p2 dies. */
    {
        std::exception_ptr p2 = capture("dropped");
        printf("held=%d\n", p2 ? 1 : 0);
    }   /* p2 destroyed here -> exactly one ~Payload */

    printf("done\n");
    return 0;
}
