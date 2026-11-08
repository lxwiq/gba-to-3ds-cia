# Third-party material

| File | Origin | License |
|------|--------|---------|
| `assets/banner_cgfx_header.bin` | CGFX banner template from bannertool (Steveice10, carstene1ns, diasurgical forks) | MIT |
| `assets/cert_chain_retail.bin` | Public certificates CA00000003, XS0000000c and CP0000000b, as embedded in makerom (3DSGuy/Project_CTR) | public certificates |
| `assets/player/player.cxi` | The gba2cia player (`player/`), built with the mGBA core (`player/third_party/mgba`, Jeffrey Pfau and contributors) | MPL-2.0 |
| `assets/template/*` | Structural bytes of a GBA Virtual Console title (NCCH header, extended header with its access descriptor, empty RomFS, AGB_FIRM config block, boot logo), as published by vcoven (Vedant Mehta) | MIT (vcoven) |

The file formats were implemented from the documentation on 3dbrew.org and
from the source code of makerom, 3dstool, bannertool, GodMode9 and
open_agb_firm. No code from those projects is copied except the binary
templates listed above.

No game data is included in this repository.
