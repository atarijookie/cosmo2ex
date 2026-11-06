| Start of CE_ETH.XIF. inet4.xdd calls the very start of the text segment as long init(struct kerinfo *, struct netinfo *),
| so this must be linked first. There's no other startup: the kernel relocates the program and clears BSS.

    .globl  _xifInit

    .text

    jmp     _xifInit
