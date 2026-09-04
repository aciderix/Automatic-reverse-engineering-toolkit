/* std::nested_exception: throw_with_nested wraps the in-flight exception (via
 * std::current_exception in the nested_exception ctor) into a type derived from both the
 * thrown type AND std::nested_exception; rethrow_if_nested uses dynamic_cast to detect the
 * nested base and rethrow_nested() re-raises the carried exception. Exercises current_exception
 * + rethrow_exception (modelled) AND __dynamic_cast (RTTI runtime, lifted libstdc++). */
#include <cstdio>
#include <stdexcept>
#include <exception>

static void inner() { throw std::runtime_error("inner"); }

static void outer() {
    try { inner(); }
    catch (...) { std::throw_with_nested(std::logic_error("outer")); }
}

static void unwrap(const std::exception& e, int depth) {
    printf("depth %d: %s\n", depth, e.what());
    try { std::rethrow_if_nested(e); }          /* dynamic_cast to nested_exception, rethrow */
    catch (const std::exception& inner) { unwrap(inner, depth + 1); }
    catch (...) { printf("depth %d: non-std nested\n", depth + 1); }
}

int main() {
    setvbuf(stdout, 0, _IONBF, 0);
    printf("start\n");
    try { outer(); }
    catch (const std::exception& e) { unwrap(e, 0); }
    printf("done\n");
    return 0;
}
