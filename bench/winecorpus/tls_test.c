#include <windows.h>
#include <stdio.h>
int main(void){
  DWORD a=TlsAlloc(), b=TlsAlloc();
  int ok_alloc = (a!=TLS_OUT_OF_INDEXES && b!=TLS_OUT_OF_INDEXES && a!=b);
  TlsSetValue(a,(LPVOID)0x1234); TlsSetValue(b,(LPVOID)0xBEEF);
  int va=(int)(intptr_t)TlsGetValue(a), vb=(int)(intptr_t)TlsGetValue(b);
  TlsFree(a); TlsFree(b);
  void*p=(void*)0xCAFEF00D;
  void*e=EncodePointer(p); void*d=DecodePointer(e);
  printf("tls_ok=%d va=%x vb=%x ptr_roundtrip=%d\n", ok_alloc, va, vb, d==p);
  return 0;
}
