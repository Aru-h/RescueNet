# Vendored three.js

three.js **0.186.1**, MIT licensed (see `LICENSE`), copied unmodified from the
npm package `three@0.186.1` (`build/three.module.js`, `build/three.core.js`).
Loaded locally by `static/dashboard/map-view3d.js` for the dashboard's optional
"3D" map view; nothing is fetched from a CDN at runtime.

To update: `npm pack three@<version>`, replace the two files and `LICENSE`
from the tarball (`package/build/`, `package/LICENSE`), and update the version
here and in the repository's `THIRD_PARTY.md`.

## Addons

`addons/` holds a few unmodified files from the same package's
`examples/jsm/` (also MIT, same `LICENSE`):

- `Line2.js`, `LineSegments2.js`, `LineGeometry.js`,
  `LineSegmentsGeometry.js`, `LineMaterial.js` (`examples/jsm/lines/`) - lines
  with a real pixel width, used for team routes (plain WebGL lines are 1px).
- `BufferGeometryUtils.js` (`examples/jsm/utils/`) - merging geometries into
  one draw call, used for the procedural buildings.

They import the bare specifier `three`, which `templates/index.html` maps to
`/static/three/three.module.js` with an import map (so every file shares one
three.js instance).
