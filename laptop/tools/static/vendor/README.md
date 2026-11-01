# Vendored front-end libraries

Local copies of the libraries the landing page uses, so the app works with
no internet connection. Each folder holds one exact version, unmodified.

| Folder | Library | Licence | Source |
| --- | --- | --- | --- |
| `gsap-3.12.5/` | GSAP 3.12.5 and its ScrollTrigger plugin | GSAP Standard "no charge" licence, https://gsap.com/standard-license (notices kept in each file) | npm `gsap@3.12.5` |
| `lenis-1.1.20/` | Lenis 1.1.20 (smooth scrolling) | MIT, see `LICENSE` | npm `lenis@1.1.20` |
| `three-r128/` | three.js r128 (`three.min.js`) | MIT, see `LICENSE` | npm `three@0.128.0` |

three.js r128 is kept apart from the newer module build in `static/three/`
because the landing page's map shader is written against r128.

Fonts (Archivo and JetBrains Mono, latin and latin-ext subsets, SIL Open
Font License 1.1) are in `static/fonts/`, with their licences beside them.
