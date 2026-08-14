/* LEVIER 1 on the GNU C++ RUNTIME -- step 3b: an exception ORIGINATING inside lifted
 * libstdc++ unwinds to the exe's catch. std::vector::at(5) on a 2-element vector throws
 * std::out_of_range from WITHIN libstdc++ (__throw_out_of_range_fmt -> __cxa_throw, a
 * DIRECT intra-module call): so __cxa_throw is host-backed (not just the exe's import) to
 * ARET's dispatcher, and the thrown out_of_range (libstdc++'s COMDAT copy of the type_info)
 * is matched against the exe's own weak COMDAT copy of the same type_info -- distinct
 * addresses because ARET lifts modules without the native loader's weak-symbol merge, so
 * the match is by MANGLED NAME (mingw __GXX_MERGED_TYPEINFO_NAMES=0), not pointer. Caught as
 * the exact type AND as std::exception&; .what() returns libstdc++'s formatted message.
 * Under Wine the SAME libstdc++ is loaded beside the exe => bit-identical. */
#include <cstdio>
#include <vector>
#include <stdexcept>
int main() {
    printf("start\n");
    std::vector<int> v;
    v.push_back(10); v.push_back(20);
    try {
        int x = v.at(5);            /* throws std::out_of_range FROM INSIDE libstdc++ */
        printf("got %d\n", x);
    } catch (const std::out_of_range& e) {
        printf("oor: %s\n", e.what());
    } catch (const std::exception& e) {
        printf("exc: %s\n", e.what());
    }
    printf("done\n");
    return 0;
}
