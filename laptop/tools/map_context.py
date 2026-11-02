"""The city around a run, built by the simulator's own map generator (step 2 of two).

Step 1 is the run itself: one 36 x 36 city, generated and simulated as always (nothing here changes it). Step 2 builds
the eight areas around it, one sector deep on every side, so the sim page can draw the city carrying on past the edge of
the run instead of stopping at it. Each area is a real city from the same generator (`./tron SEED 1 1 --live`, read
from its STATICMAP and STATICENV lines), picked and turned so that it meets its neighbours well:

  - water lines up across each edge: a coast strip in the run meets a coast strip in the area beside it, so the two
    form one channel with land on both sides (never open sea), and a river leaving the run carries on;
  - roads line up: an area is preferred when its streets reach the edge where the run's streets do;
  - what still does not meet is joined: a road leaving the run (or an area) is carried into the next area until it
    reaches a street there, and a river is widened across the seam where the two sides differ by a cell or two.
    Buildings in the way of a joined road or river are dropped.

Nothing in the surroundings is simulated. Damage is not taken from the areas' own earthquakes: the page colours them
from the run's damage-by-distance curve around the run's epicentre (static/shared/rn-context.js).

Usage: python3 laptop/tools/map_context.py SEED [--binary ./tron] [--pool 400] [--cache DIR] > context.json
Output (cells in the run's coordinates, so they run from -36 to 71):
  {"seed", "ring": 36, "tiles": [{"dx", "dy", "seed", "turn"}], "roads": [[x, y, class]], "water": [[x, y]],
   "bridges": [[x, y]], "buildings": [[x, y, w, h]]}   (buildings by top-left cell)
"""
import argparse, json, os, pathlib, subprocess, sys

N = 36
POOL_BASE = 700000   # pool cities are seeds POOL_BASE, POOL_BASE + 1, ...: far from any seed a run would use


def generate(binary, seed):
    """One generated city: water cells, road classes and buildings (top-left), from a one-step run."""
    out = subprocess.run([binary, str(seed), '1', '1', '--live'], capture_output=True, text=True, check=True).stdout
    env = smap = None
    for line in out.splitlines():
        if line.startswith('STATICENV ') and env is None:
            env = json.loads(line[10:])
        elif line.startswith('STATICMAP ') and smap is None:
            smap = json.loads(line[10:])
        if env and smap:
            break
    water, road = set(), {}
    for y, row in enumerate(smap['rows']):
        for k in range(0, len(row), 8):
            x, fl = k // 8, int(row[k:k + 2], 16)
            if fl & 2:
                water.add((x, y))
            if (fl >> 2) & 7:
                road[(x, y)] = (fl >> 2) & 7
    bld = []
    for b in env['buildings']:
        w, h = b.get('w', 1), b.get('h', 1)   # STATICENV gives the footprint's centre (x0 + w / 2)
        bld.append((b['x'] - w // 2, b['y'] - h // 2, w, h))
    return {'seed': seed, 'water': water, 'road': road, 'bld': bld, 'epicenter': env.get('epicenter')}


def pool(binary, size, cache):
    """The cities step 2 picks from, cached on disk: generating 400 takes about half a minute."""
    path = pathlib.Path(cache) / f'pool_{POOL_BASE}_{size}.json'
    if path.is_file():
        raw = json.loads(path.read_text())
    else:
        raw = []
        for i in range(size):
            c = generate(binary, POOL_BASE + i)
            raw.append({'seed': c['seed'], 'water': sorted(c['water']), 'road': [[x, y, v] for (x, y), v in c['road'].items()], 'bld': c['bld']})
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(raw, separators=(',', ':')))
    return [{'seed': r['seed'], 'water': set(map(tuple, r['water'])), 'road': {(x, y): v for x, y, v in r['road']}, 'bld': [tuple(b) for b in r['bld']]} for r in raw]


# the eight ways to lay a square city down (turns and mirror images); a generated city stays one after any of them
TURNS = [lambda x, y: (x, y), lambda x, y: (N - 1 - x, y), lambda x, y: (x, N - 1 - y), lambda x, y: (N - 1 - x, N - 1 - y),
         lambda x, y: (y, x), lambda x, y: (N - 1 - y, x), lambda x, y: (y, N - 1 - x), lambda x, y: (N - 1 - y, N - 1 - x)]


def turned(c, t):
    f = TURNS[t]
    out = {'water': {f(x, y) for x, y in c['water']}, 'road': {f(x, y): v for (x, y), v in c['road'].items()}, 'bld': []}
    for x, y, w, h in c['bld']:
        xs, ys = zip(*(f(x + i, y + j) for i in range(w) for j in range(h)))
        out['bld'].append((min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1))
    return out


_EDGE = {}


def edge(c, side, depth=0):
    """What lies along one side of a 36 x 36 city, `depth` cells in: [(water?, road class or 0)] for i = 0..35."""
    key = (id(c), side, depth)
    if key in _EDGE:
        return _EDGE[key]
    cell = {'L': lambda i: (depth, i), 'R': lambda i: (N - 1 - depth, i), 'T': lambda i: (i, depth), 'B': lambda i: (i, N - 1 - depth)}[side]
    _EDGE[key] = [(cell(i) in c['water'], c['road'].get(cell(i), 0)) for i in range(N)]
    return _EDGE[key]


_ENDS = {}


def road_ends(c, side):
    """Positions along a side where a street runs out to within two cells of it."""
    if (id(c), side) in _ENDS:
        return _ENDS[(id(c), side)]
    ends = set()
    for i in range(N):
        for d in range(3):
            if edge(c, side, d)[i][1] and edge(c, side, d + 1)[i][1]:
                ends.add(i)
                break
    _ENDS[(id(c), side)] = ends
    return ends


OPP = {'L': 'R', 'R': 'L', 'T': 'B', 'B': 'T'}
STEP = {'L': (-1, 0), 'R': (1, 0), 'T': (0, -1), 'B': (0, 1)}


def fit(a, side, b):
    """How well city b, laid beside city a on a's `side`, meets it: water that does not line up costs a lot, streets
    that do line up earn a little."""
    ea, eb = edge(a, side), edge(b, OPP[side])
    miss = sum(1 for i in range(N) if ea[i][0] != eb[i][0])
    ra, rb = road_ends(a, side), road_ends(b, OPP[side])
    meet = sum(1 for i in ra if any(j in rb for j in (i - 1, i, i + 1)))
    return -6 * miss + 2 * meet - (3 if not ra and rb else 0)


def build(seed, binary, size, cache):
    run = generate(binary, seed)
    cities = pool(binary, size, cache)
    placed = {(0, 0): run}
    laid = [[turned(c, t) for t in range(8)] for c in cities]   # every city in every way it can be laid down, once
    tiles, used = [], set()
    # the four areas beside the run first, then the corners (which then have two neighbours to meet)
    for dx, dy in [(-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (1, -1), (-1, 1), (1, 1)]:
        best = None
        for c, ways in zip(cities, laid):
            if c['seed'] in used:
                continue
            for t in range(8):
                ct = ways[t]
                score = 0
                for side, (sx, sy) in STEP.items():   # every neighbour already placed
                    nb = placed.get((dx + sx, dy + sy))
                    if nb is not None:
                        score += fit(nb, OPP[side], ct)
                if best is None or score > best[0]:
                    best = (score, c['seed'], t, ct)
        used.add(best[1])
        placed[(dx, dy)] = best[3]
        tiles.append({'dx': dx, 'dy': dy, 'seed': best[1], 'turn': best[2], 'fit': best[0]})

    # everything in the run's coordinates
    water, road, bld = set(), {}, []
    for (dx, dy), c in placed.items():
        if (dx, dy) == (0, 0):
            continue
        ox, oy = dx * N, dy * N
        water |= {(x + ox, y + oy) for x, y in c['water']}
        road.update({(x + ox, y + oy): v for (x, y), v in c['road'].items()})
        bld += [(x + ox, y + oy, w, h) for x, y, w, h in c['bld']]
    inside = lambda x, y: 0 <= x < N and 0 <= y < N
    in_ring = lambda x, y: -N <= x < 2 * N and -N <= y < 2 * N and not inside(x, y)
    run_road = {k: v for k, v in run['road'].items()}
    run_water = run['water']
    allroad = lambda x, y: run_road.get((x, y), 0) if inside(x, y) else road.get((x, y), 0)
    allwater = lambda x, y: (x, y) in run_water if inside(x, y) else (x, y) in water

    # join what still does not meet, never inside the run
    carved = set()
    for (dx, dy), c in placed.items():
        ox, oy = dx * N, dy * N
        for side, (sx, sy) in STEP.items():
            if (dx + sx, dy + sy) not in placed:
                continue
            for i in range(N):
                # the boundary cell on this city's side and the first cell across it
                bx, by = {'L': (0, i), 'R': (N - 1, i), 'T': (i, 0), 'B': (i, N - 1)}[side]
                bx += ox; by += oy
                # water: a channel or river cell at the edge carries one cell across if the other side is land
                if allwater(bx, by) and not allwater(bx + sx, by + sy) and in_ring(bx + sx, by + sy):
                    if allwater(bx + 2 * sx, by + 2 * sy) or any(allwater(bx + sx + px, by + sy + py) for px, py in ((sy, sx), (-sy, -sx))):
                        water.add((bx + sx, by + sy)); carved.add((bx + sx, by + sy))
                # roads: a street ending within two cells of the edge is carried on until it meets one across it
                end = None
                for d in range(3):
                    px, py = bx - sx * d, by - sy * d
                    if allroad(px, py) and allroad(px - sx, py - sy):
                        end = (px, py, allroad(px, py)); break
                if end is None:
                    continue
                path, x, y = [], end[0] + sx, end[1] + sy
                for _ in range(8):
                    if allroad(x, y) and not (x, y) in [(p[0], p[1]) for p in path] and (abs(x - end[0]) + abs(y - end[1])) > 2:
                        break
                    path.append((x, y)); x += sx; y += sy
                else:
                    continue   # no street within reach across the edge: leave it ending, as the run's map does
                cls = end[2]
                for x, y in path:
                    if not in_ring(x, y):
                        continue   # never draw inside the run: its map is the simulation's own
                    if allwater(x, y) and cls < 3:
                        break
                    road[(x, y)] = max(road.get((x, y), 0), cls); carved.add((x, y))
    # buildings in the way of what was joined, or over the edge of the ring, go
    def clear(b):
        x, y, w, h = b
        cells = [(x + i, y + j) for i in range(w) for j in range(h)]
        return all(in_ring(*p) and p not in carved and p not in water for p in cells)   # the generator itself puts some roads under buildings
    bld = [b for b in bld if clear(b)]
    bridges = sorted(p for p in road if p in water)
    return {'seed': seed, 'ring': N, 'tiles': tiles,
            'roads': sorted([x, y, v] for (x, y), v in road.items() if in_ring(x, y)),
            'water': sorted([x, y] for x, y in water if in_ring(x, y)),
            'bridges': [list(p) for p in bridges if in_ring(*p)],
            'buildings': sorted(list(b) for b in bld)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('seed', type=int)
    here = pathlib.Path(__file__).resolve().parent
    ap.add_argument('--binary', default=str(here.parent.parent / 'tron'))
    ap.add_argument('--pool', type=int, default=400)
    ap.add_argument('--cache', default=str(here / 'cache' / 'map_context'))
    a = ap.parse_args()
    json.dump(build(a.seed, a.binary, a.pool, a.cache), sys.stdout, separators=(',', ':'))


if __name__ == '__main__':
    main()
