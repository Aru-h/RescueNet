# Vendored three.js

three.js **0.186.1**, MIT licensed (see `LICENSE`), copied unmodified from the
npm package `three@0.186.1` (`build/three.module.js`, `build/three.core.js`).
Loaded locally by `static/dashboard/map-view3d.js` for the dashboard's optional
"3D" map view; nothing is fetched from a CDN at runtime.

To update: `npm pack three@<version>`, replace the two files and `LICENSE`
from the tarball (`package/build/`, `package/LICENSE`), and update the version
here and in the repository's `THIRD_PARTY.md`.
