| File | SHA-256 | Size | Arch | Linker / built (UTC) | Image base | Entry VA | Notes |
|---|---|---|---|---|---|---|---|
| BlackOps.exe | `2ba4a04e81de2b60582da0e4489efde21e6cb07904a223bae633b6ccaebbf759` | 8,099,608 | PE32 0x014c GUI | link 9.0 / 2011-09-01 | 0x00400000 | 0x0096da57 (`.text`) | Authenticode table present; TLS (0 callbacks); relocs stripped; LAA; overlay 3352 B |
| binkw32.dll | `ad55ec64a893f233ac648ac974562f53736003b17bfd14527e66b3ffc8c2cdcb` | 174,080 | PE32 0x014c GUI | link 8.0 / 2009-04-03 | 0x18000000 | 0x18001d80 (`.text`) | 1976 relocs; no LAA |
| steam_api.cdx | `9efe2c8336325f6f186af073213b84f6258643517d175bc8432e196b70722394` | 124,712 | PE32 0x014c GUI | link 8.0 / 2010-08-25 | 0x3b400000 | 0x3b406922 (`.text`) | Authenticode table present; 2091 relocs; no LAA; overlay 5928 B |
| steam_api.dll | `d1757479242125561628f75864eaef3118f7bf5b6b136cf3681c21e2da612b48` | 255,776 | PE32 0x014c GUI | link 12.0 / 2016-12-12 | 0x10000000 | 0x100ad570 (`WUS1`) | Authenticode table present; 3 relocs; no LAA; overlay 1312 B |

**BlackOps.exe — sections**

| Name | VA | VSize | RawSize | Flags | Entropy |
|---|---|---|---|---|---|
| `.text` | 0x00401000 | 5,904,912 | 5,905,408 | RXc | 6.727 |
| `.rdata` | 0x009a3000 | 1,869,498 | 1,869,824 | Rd | 4.49 |
| `.data` | 0x00b6c000 | 61,814,468 | 238,592 | RWd | 3.507 |
| `.tls` | 0x04660000 | 109 | 512 | RWd | 0.037 |
| `.rodata` | 0x04661000 | 1,904 | 2,048 | Rd | 3.728 |
| `.version` | 0x04662000 | 4 | 512 | Rd | 0.082 |
| `.rsrc` | 0x04663000 | 78,020 | 78,336 | Rd | 7.86 |

Rich header: none

Imports (334): steam_api.dll (19), WINMM.dll (11), WSOCK32.dll (11), binkw32.dll (18), d3d9.dll (3), d3dx9_43.dll (6), DSOUND.dll (2), KERNEL32.dll (166), USER32.dll (56), GDI32.dll (7), ADVAPI32.dll (12), SHELL32.dll (2), ole32.dll (7), XINPUT1_3.dll (3), PSAPI.DLL (1), WS2_32.dll (10)

Exports: 0

**binkw32.dll — sections**

| Name | VA | VSize | RawSize | Flags | Entropy |
|---|---|---|---|---|---|
| `.text` | 0x18001000 | 4,428 | 4,608 | RXc | 6.253 |
| `BINKY12` | 0x18003000 | 432 | 512 | RXc | 5.415 |
| `BINKY16` | 0x18004000 | 2,264 | 2,560 | RXc | 5.873 |
| `BINKP8` | 0x18005000 | 2,297 | 2,560 | RXc | 6.174 |
| `BINK16` | 0x18006000 | 4,107 | 4,608 | RXc | 6.079 |
| `BINK32` | 0x18008000 | 4,938 | 5,120 | RXc | 6.345 |
| `BINK` | 0x1800a000 | 94,098 | 94,208 | RXc | 6.571 |
| `BINKBSS` | 0x18021000 | 21,280 | 0 | RWu | 0.0 |
| `.rdata` | 0x18027000 | 5,881 | 6,144 | Rd | 5.404 |
| `.data` | 0x18029000 | 152 | 512 | RWd | 1.061 |
| `BINKCONS` | 0x1802a000 | 20,624 | 20,992 | Rd | 6.167 |
| `BINKDATA` | 0x18030000 | 16,536 | 16,896 | RWd | 7.576 |
| `.rsrc` | 0x18035000 | 8,568 | 8,704 | Rd | 4.731 |
| `.reloc` | 0x18038000 | 4,852 | 5,120 | Rd | 6.056 |

Rich header (top by object count): import (linker-generated) build 0 ×116, prod 0x0071 build 50727 ×36, VS2003 C build 4035 ×9, VS2005 C++ build 50727 ×3, VS2005 C build 50727 ×1, VS2005 MASM build 50727 ×1, prod 0x007a build 50727 ×1, VS2005 export build 50727 ×1

Imports (110): USER32.dll (28), GDI32.dll (12), KERNEL32.dll (39), WINMM.dll (12), mss32.dll (19, delay)

Exports: 72

Version info: CompanyName=RAD Game Tools, Inc.; ProductName=Bink and Smacker; FileDescription=RAD Video Tools; FileVersion=1.9n

**steam_api.cdx — sections**

| Name | VA | VSize | RawSize | Flags | Entropy |
|---|---|---|---|---|---|
| `.text` | 0x3b401000 | 74,668 | 77,824 | RXc | 6.552 |
| `.rdata` | 0x3b414000 | 16,186 | 16,384 | Rd | 5.507 |
| `.data` | 0x3b418000 | 12,932 | 8,192 | RWd | 1.784 |
| `.rsrc` | 0x3b41c000 | 1,932 | 4,096 | Rd | 3.971 |
| `.reloc` | 0x3b41d000 | 7,652 | 8,192 | Rd | 4.48 |

Rich header: none

Imports (86): KERNEL32.dll (82), ADVAPI32.dll (3), SHELL32.dll (1)

Exports: 53

Version info: CompanyName=Valve Corporation; ProductName=Steam Client API; FileDescription=Steam Client API; FileVersion=1, 0, 0, 1; OriginalFilename=steam_api.dll

**steam_api.dll — sections**

| Name | VA | VSize | RawSize | Flags | Entropy |
|---|---|---|---|---|---|
| `WUS0` | 0x10001000 | 458,752 | 0 | RWXu | 0.0 |
| `WUS1` | 0x10071000 | 249,856 | 247,808 | RWXd | 7.932 |
| `.rsrc` | 0x100ae000 | 8,192 | 5,632 | RWd | 4.786 |

Rich header (top by object count): prod 0x00e0 build 21005 ×234, import (linker-generated) build 0 ×133, prod 0x00e1 build 21005 ×71, prod 0x00e4 build 40629 ×71, prod 0x00df build 21005 ×30, prod 0x00cb build 65501 ×15, VS2005 import build 50727 ×2, prod 0x00dc build 40629 ×1

Imports (12): KERNEL32.DLL (5), ADVAPI32.dll (1), ntdll.dll (1), PSAPI.DLL (1), SHELL32.dll (1), SHLWAPI.dll (1), USER32.dll (1), WS2_32.dll (1)

Exports: 88

Version info: CompanyName=Valve Corporation; ProductName=Steam Client API; FileDescription=Steam Client API; FileVersion=1, 0, 0, 1; OriginalFilename=steam_api.dll
