# REQUIREMENTS (the website, the sim page and its maps)

One place for everything the RescueNet pages must do. When a page and this file disagree, either the page is wrong or
this file is out of date: fix one of them in the same commit. Status: **DONE**, **PARTLY** (says what is missing),
**TO DO**, **OPEN** (a decision not made yet).

Research behind the map rules: the "Map notation" tab of the research doc. Code that implements them:
`laptop/tools/static/shared/rn-notation.js` (colours, classes, symbols, legend), used by every map.

## 1. Who the pages are for

| ID | Requirement | Status |
|---|---|---|
| A1 | A non-technical visitor (judge, press, public) understands what they are looking at within a few seconds, without reading a manual: a city, damaged buildings, teams moving, sites being worked and finished. | OPEN (sim page not built) |
| A2 | A technical reader (rescue coordinator, engineer) can read exact positions, IDs, timings, injected faults versus what was detected, and the radio traffic, one click away from the first view. | OPEN |
| A3 | Both audiences see the same data. No view shows something the others hide, except the ground-truth switch (H3). | OPEN |

## 2. Honesty (applies to every page, text and drawing)

| ID | Requirement | Status |
|---|---|---|
| H1 | No claims of lives saved, of "catching" something earlier than human operators, or of a working multi-board LoRa network. The board demo shows that the code runs on the STM32N6570-DK under µT-Kernel, on one board. See `docs/CLAIMS.md`. | DONE on the landing page |
| H2 | Simulated runs are labelled as simulated; real-city runs say which parts are real (map, damage) and which are simulated (teams, victims, faults). | PARTLY (run list cards do this; sim page TO DO) |
| H3 | Ground truth (injected faults, incidents no team knows about yet) is shown only behind a clearly labelled switch, and is never mixed into what the system "knows". | TO DO |
| H4 | Where a field standard has no honest equivalent in the simulator, the map leaves it out instead of imitating it: no D (deceased) victims, no A/B triage letters, no invented team IDs or INSARAG classifications. | DONE in the notation rules; TO DO on the sim page |
| H5 | Real data is credited as its licence requires (Liu et al. CC BY 4.0; OpenStreetMap ODbL; Copernicus EMS © European Union; Microsoft ODbL). | DONE on the run list |

## 3. Map notation (field standards)

Follows the INSARAG Guidelines 2020. A new version takes effect on 1 January 2027: re-check this section then.

| ID | Requirement | Standard | Status |
|---|---|---|---|
| N1 | Building damage, 0–100 in the simulator: 75+ red `#FF0000`, 50–74 orange `#E69800`, 25–49 yellow `#FFFF00`, below 25 an ink outline. Legend names the simulator's own classes (severe, moderate, slight). | Copernicus EMS grading colours | DONE (run list); TO DO (sim 2D and 3D) |
| N2 | The standard colours never follow the page's clock palette; only outlines take the page's ink, and every fill has an outline. | Contrast | DONE |
| N3 | Epicentre: a star. | USGS ShakeMap | DONE (run list) |
| N4 | Blocked road: crimson diamond with a white bar. | Copernicus "road barrier" | DONE (symbol) |
| N5 | Safe zones are "assembly points"; spontaneous gathering (real-city data) is magenta and drawn differently. | OCHA icon name; Copernicus "spontaneous site" | DONE (run list) |
| N6 | Incidents that could hold a live rescue (collapse, confined space, survivor signal, mass casualty) are worksites with an INSARAG ID: sector letter, hyphen, number in order of discovery (A-3). Numbering is computed over the whole run, so IDs never change while scrubbing. | INSARAG Vol II Man B 5.5 | TO DO |
| N7 | Sector letters skip I and O. The simulated grid (36 × 36) is one sector, A; the sectors around it are B, C, … the same size, nearest first. Sectors are a display overlay and the legend says so (the simulator does not sectorise). | INSARAG 5.5.3 | TO DO |
| N8 | Worksite state: outlined (no team yet), blue (team working), green with a line through the whole marking (complete). | ICMS colours; INSARAG 6.2 completion line | DONE (symbol); TO DO (sim) |
| N9 | Live victims shown as L-n. | INSARAG victim marking | TO DO |
| N10 | Gas, fire and chemical incidents are hazards written in plain words (GAS, FIRE, CHEM), not worksites. | INSARAG 6.2 | TO DO |
| N11 | Sector assessment, supplies, evacuation and road-clearing tasks are "other tasks" (small triangle), not worksites. | | DONE (symbol); TO DO (sim) |
| N12 | Never rely on colour alone: every status also differs in shape or line. | Colour-vision research | DONE in the symbols |
| N13 | Teams are "Team 4" with a status shape; no invented country codes. | | TO DO |

## 4. Pages and how they connect

```
Landing -> How do you want to connect?
   |- Simulation        -> Mission setup -> Sim page (live)
   |- Board             -> Connect a board -> Sim page (live, fed by the board)
   '- Watch a recorded run -> Run list -> Sim page (replay)
```

| ID | Requirement | Status |
|---|---|---|
| P1 | Landing page with the Simulation / Board choice and a "Watch a recorded run" link. In the app it opens `/replay`; as a hosted page it opens the published run list. | DONE |
| P2 | Run list: featured runs (real cities first, then our own scenarios), then "Your runs" (closed by default, live count), maps the same size in both. | DONE |
| P3 | One sim page, `/sim?run=…`, for replay; live runs and boards later feed the same page. "Take over from here" turns a replay into a live run without leaving the page. | TO DO |
| P4 | The app's `/` becomes the landing page; the old dashboard is removed once the sim page replaces it. | TO DO |
| P5 | Mission setup step after choosing Simulation: city (seed, or another at random), mission length (2, 6, 12 or 24 h), what goes wrong (none, movement, radio, supplies, mixed, rough), radio loss under Advanced; a summary and the matching `./tron` command beside it. Only options the simulator really takes (`/api/run-live`: seed, ticks, fault_profile, link_loss); team count is shown as fixed. | PARTLY (built; Start opens the current dashboard until the sim page reads the choices) |

## 5. Sim page

| ID | Requirement | Status |
|---|---|---|
| S1 | Two map views: 3D (default on desktop) and flat 2D (default on phones, and the fallback when WebGL is missing). The 2.5D view is removed. | TO DO |
| S2 | Both views reuse the old dashboard's code (`static/dashboard/`: camera, map model, 3D view, replay, routes, selection, timeline, consensus ring). The change is the look and the notation, not a rewrite. | TO DO |
| S3 | Camera: the old dashboard's controls (drag to rotate, tilt between 15° and straight down, right-drag to pan, wheel to zoom at the cursor, Top, Reset, follow a team, fly to a site, Esc stops), plus a quick return to the standard angle. | TO DO |
| S4 | Timeline: play, pause, step, speed, scrubber with markers (worksite identified, worksite complete, detections), jump to the next event. Mission clock as "T+2 h 59 min" (one tick is 36 s). | TO DO |
| S5 | Teams panel: status in words, stamina and equipment as two labelled meters (dashed below 30, where the simulator sends a team to rest or restock), current worksite, what peers detected; injected faults only under ground truth (H3). Rows grow to fill the panel when there are few teams and shrink to a compact size before the list scrolls. | TO DO (frames carry `equipment` since this change; older recordings show "–") |
| S6 | Technical panels one click away: detection timeline (injected versus detected), peer-network ring, radio counters, raw tables. | TO DO |
| S7 | Event list in plain sentences built from the frames ("T+1 h 12 min: A-3 identified, 4 live victims reported"). | TO DO |
| S8 | Links to a moment: `/sim?run=…&t=…&view=flat`. | TO DO |
| S9 | Keyboard: Space play/pause, ←/→ one tick, Shift+←/→ ten, J next event, 1–0 pick a team, Esc back. | TO DO |
| S10 | Phone: map and timeline, teams as a drawer. | TO DO |

## 6. Map edge, zones and exits (OPEN)

The current map is one 36 × 36 grid. Real cities (Antakya, Kathmandu) are irregular, larger and made of several zones.

| ID | Question | Current thinking | Status |
|---|---|---|---|
| E1 | How is the edge of the simulated area shown? | The area is outlined in green (Copernicus area of interest). Around it, one sector deep on every side, the city carries on faded: `static/shared/rn-context.js` continues the roads, water and damage at the edge (roads leaving the map carry on at their class, a coast stays sea, a river keeps meandering, damage follows the run's own damage-by-distance curve). Drawn on the page, not simulated, not selectable, and never called the simulator's output. | PARTLY (mockup and demo candidates; sim page TO DO) |
| E2 | How do several zones fit together (e.g. Kathmandu's 19 sectors)? | One simulated grid is one INSARAG sector (A in a single-grid run). A multi-zone city is several grids, one sector each, with its own letter and boundary. | OPEN |
| E4 | Bigger maps in sim mode (later version). Board mode stays one 36 × 36 sector. Sim mode is sized in whole sectors (1 × 1, 2 × 2, 3 × 3), each sector its own cell of 10 teams running today's consensus (it is written for at most 16 teams), with cross-sector hand-off after. Needs: the grid size as a laptop build option, the generator's building and junction limits scaled (they are laptop-only storage, so the board is unaffected), `check-state-hash` still bit-for-bit for one sector, and a fresh evaluation before any claim about larger maps. | | DEFERRED |
| E3 | Exit tunnels (evacuation gateways). The old dashboard's tunnels used brute-forced paths and caused problems. | Not carried over. Redesign: exits sit where real roads cross the area's edge, derived from the road network, not searched for. | OPEN; old tunnel code excluded |

## 7. Platform

| ID | Requirement | Status |
|---|---|---|
| T1 | Works fully offline: every library and font is served by the app (`laptop/tools/static/`). | DONE (landing, run list) |
| T2 | Dashboard install: `pip install -r laptop/tools/requirements-app.txt` (Flask, pyserial), with an offline wheelhouse option. | DONE |
| T3 | Hosted copies (claude.ai artifacts) of the landing page and run list work without the app and say what needs the app. | DONE |
| T4 | Every page follows the clock palette (`static/shared/rn-sky.js`) and works at phone width without sideways scrolling. | DONE (landing, run list) |
| T5 | Respect "reduce motion". | PARTLY (old 3D view has it) |

## 8. Fatigue estimate: "tiring faster than expected" (OPEN, planned)

Idea: predict how fast each team should tire and recover, learn each team's own rates as it works, and flag a team
whose observed fatigue keeps running well ahead of that prediction. The same expected-versus-observed check the
detector already uses for other faults (and the mission execution twin uses for work left), applied to energy.
In the simulator stamina comes from a fixed formula (`core/team.c`: `change_stamina`, `team_travel_stamina_cost`,
the fitness tiers' soft floors), so the check only means something once a fault exists that changes it.

| ID | Step | Status |
|---|---|---|
| F1 | Research real work and rest data for rescue and fire crews (firefighter rehab standards such as NFPA 1584, USAR field and physiology studies): energy use by activity (heavy work, travel with load, standby), work-before-rest limits, recovery rates, heat and fitness effects. Record sources and numbers in the research doc, with their limits. | TO DO |
| F2 | Fatigue model: drain per tick by activity (work, travel, carrying load), recovery per tick while resting, starting rates from F1. Each team's rates are learned online (a simple filter on its own history), so a fitter team is not flagged for being fit. | TO DO |
| F3 | What counts as "observed" outside the simulator: time worked versus rested, slowing down on comparable terrain, optionally wearables (heart rate). Nobody reports a stamina percentage in the field, so the model must work from these. | TO DO |
| F4 | A new fault in the simulator that makes a team tire faster (standing in for injury, heat stress or illness), so the check can be measured like the other fault classes: detection rate, false alarms, delay, paired by seed. | TO DO |
| F5 | Wording: the alert says "tiring faster than expected", never "stamina fault confirmed"; results go into `docs/CLAIMS.md` as SIM-ONLY until tested against real data. | TO DO |
| F6 | On the sim page: the stamina meter shows the expected band behind the observed value, and the team row says "tiring faster than expected" when the gap stays large. | TO DO (after F1–F4) |
