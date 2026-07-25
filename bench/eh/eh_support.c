/* Bridges a clang MSVC-ABI C++ object to Wine's msvcrt exports:
   - __CxxThrowException@8 (clang stdcall-decorated) tail-jumps to the msvcrt import
     __CxxThrowException (exported undecorated as _CxxThrowException);
   - type_info::vftable (??_7type_info@@6B@) is not exported by msvcrt, but a throw/catch
     only compares the mangled type NAMES held in the type descriptor, never calling
     through this vtable, so a dummy definition satisfies the linker. */
__asm__(
".text\n"
".globl \"__CxxThrowException@8\"\n"
"\"__CxxThrowException@8\":\n"
"  jmp __CxxThrowException\n"
".section .rdata,\"dr\"\n"
".globl \"??_7type_info@@6B@\"\n"
"\"??_7type_info@@6B@\":\n"
"  .long 0\n"
);
