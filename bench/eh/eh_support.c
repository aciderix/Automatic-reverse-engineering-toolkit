/* type_info::vftable (??_7type_info@@6B@) is referenced by clang's RTTI type descriptors
   but not provided by the mingw msvcrt import lib. A throw/catch only compares the mangled
   type NAMES in the descriptor, never calling through this vtable, so a dummy definition
   satisfies the linker. (All other EH runtime symbols come from the import libs.) */
__asm__(
".section .rdata,\"dr\"\n"
".globl \"??_7type_info@@6B@\"\n"
"\"??_7type_info@@6B@\":\n"
"  .long 0\n"
);
