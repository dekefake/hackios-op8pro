/* Bring-up stub 1: same Linux-style header as Mu-Silicium's BootShim, then spin. Touches nothing, resets nothing. */
_Head:
  nop                   /* (BootShim: adr x1, _Payload) */
  b     _Start
  .quad FD_BASE         /* Text Offset */
  .quad FD_SIZE         /* Image Size */
  .quad 0               /* Flags */
  .quad 0
  .quad 0
  .quad 0
  .ascii "ARM\x64"      /* ARM64 Magic */
  .long 0
_Start:
  wfe
  b     _Start
