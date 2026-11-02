# Vendored three.js

three.js **0.186.1**, MIT licensed (see `LICENSE`), copied unmodified from the
npm package `three@0.186.1` (`build/three.module.js`, `build/three.core.js`).
Loaded locally by the sim page (`templates/sim.html`) for its 3D view; nothing
is fetched from a CDN at runtime.

To update: `npm pack three@<version>`, replace the two files and `LICENSE`
from the tarball (`package/build/`, `package/LICENSE`), and update the version
here and in the repository's `THIRD_PARTY.md`.

## Addons

`addons/` holds one unmodified file from the same package's `examples/jsm/`
(also MIT, same `LICENSE`):

- `OrbitControls.js` (`examples/jsm/controls/`) - drag to rotate, right-drag
  to pan, scroll to zoom; the sim page's 3D camera (`templates/sim.html`).

It imports the bare specifier `three`, which `templates/sim.html` maps to
`/static/three/three.module.js` with an import map (so every file shares one
three.js instance). The line, geometry-merging and other addons the old
dashboard used were removed with it; copy them from the npm package again
if something needs them.
