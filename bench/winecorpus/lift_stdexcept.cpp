/* LEVIER 1 on the GNU C++ RUNTIME -- step 3: Itanium C++ EH THROUGH lifted libstdc++.
 * The path that unifies the EH brick (aret_cxa_* / the ARET dispatcher, doc 71) with
 * lifting the real libstdc++-6.dll: we build a std:: exception (std::string +
 * std::runtime_error) with LIFTED libstdc++ code, throw it, and __cxa_throw routes to
 * ARET's HLE dispatcher -- NOT the lifted libgcc DWARF unwinder (shared-stack
 * incompatible; loader override) -- which matches std::runtime_error against
 * catch(const std::exception&) by the subtype rule (ABI vtables found in the lifted
 * EXPORTS), runs the catch, and calls the virtual what() back into lifted libstdc++.
 * Under Wine (the oracle) the SAME libstdc++ is loaded beside the exe => bit-identical.
 *
 * Prerequisites proven along the way: DuplicateHandle of the current-thread pseudo-handle
 * (lifted libwinpthread pthread init, taken on the first throw), and EH-family routing to
 * the HLE shims. Deterministic => bit-identical. */
#include <cstdio>
#include <stdexcept>
#include <string>

int main() {
    printf("start\n");
    try {
        std::string msg = "boom-";
        msg += "42";
        throw std::runtime_error(msg);      /* constructed BY lifted libstdc++ */
    } catch (const std::exception& e) {      /* caught as a BASE (subtype match) */
        printf("caught: %s\n", e.what());    /* virtual what() into lifted libstdc++ */
    }
    /* A second, distinct type caught exactly, to exercise more of the matcher. */
    try {
        throw std::logic_error("logic");
    } catch (const std::logic_error& e) {
        printf("logic: %s\n", e.what());
    }
    printf("done\n");
    return 0;
}
