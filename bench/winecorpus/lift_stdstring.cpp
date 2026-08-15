/* LEVIER 1 on the GNU C++ RUNTIME -- guard for the __thiscall import-thunk callee-pop.
 * A std::string LONGER than 15 chars takes libstdc++'s heap path: basic_string::_M_construct
 * calls _M_create(size_type&, size_type) -- a __thiscall MEMBER function (this in ecx, two
 * stack args, `ret 8`) reached through an import thunk `jmp [IAT slot]`. The caller must
 * honour that `ret 8` pop; ARET's tail-call pop propagation only followed DIRECT `jmp func`,
 * not the `jmp [IAT]` thunk resolving to a lifted export, so every such caller left esp 8
 * low -- the string reloaded its length from the drifted stack slot as GARBAGE and wrote the
 * NUL terminator out of bounds (measured on jsoncpp's Json::LogicError(const std::string&)).
 * Here: build a >15-char message, copy it into a std::runtime_error (the copy runs _M_create
 * in lifted libstdc++), throw, catch, print length + text. Under Wine the SAME libstdc++ is
 * loaded beside the exe => bit-identical. A drift corrupts the length/text loudly. */
#include <cstdio>
#include <string>
#include <stdexcept>
int main() {
    printf("start\n");
    try {
        std::string msg = "this message is definitely longer than fifteen characters";
        printf("len=%d\n", (int)msg.size());
        throw std::runtime_error(msg);      /* copy ctor -> _M_create (heap, __thiscall thunk) */
    } catch (const std::exception& e) {
        std::string what = e.what();          /* another heap std::string copy */
        printf("caught(%d): %s\n", (int)what.size(), what.c_str());
    }
    printf("done\n");
    return 0;
}
