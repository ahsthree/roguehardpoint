# roguehardpoint

A timed hardpoint "battle for control" plugin for [BZFlag](https://www.bzflag.org/) servers, built for **Rogue wars** (everyone on the rogue team).

Every few minutes the server picks a random hardpoint, announces it, counts down, and lets it go live for a short window. Hold it alone and you score. Share it with another tank and nobody does.

It uses the **same `hardpoint` map object** as the original [hardpoint plugin](https://github.com/ahsthree/hardpoint), so existing maps work unchanged.

## How it plays

1. Every `_rogueHardPointInterval` seconds (default 5 minutes, start to start), a **random** hardpoint is picked. It is never the same one twice in a row, so nobody can camp the next one.
2. The server announces it by name:
   ```
   Go to the red hardpoint! It goes live in...
   10
   9
   8
   ...
   1
   The red hardpoint is LIVE! Hold it alone for 1 point per second (60 seconds).
   ```
3. From the announcement onward, a star of sky shots fires inward at the hardpoint from 50 units above it, so players can see where to go.
4. While it is live (`_rogueHardPointTime` seconds, default 60):
   - **Exactly one** living rogue tank inside: that player gets **+1 personal score per second**.
   - **Two or more** tanks inside: **contested**, so nobody scores.
   - **Nobody** inside: nobody scores.
5. When time is up, the server announces who earned the most points, then waits for the next interval.

The live window is a fixed clock. It never pauses or extends, so a contested hardpoint just burns time.

## Requirements

- A BZFlag server (bzfs) built with plugin support
- The BZFlag source tree or headers (for `bzfsAPI.h`)
- A C++ compiler (g++ or similar)

## Build

Adjust the include path to your BZFlag source tree:

```
g++ -shared -fPIC -I/path/to/bzflag/include -o roguehardpoint.so roguehardpoint.cpp
```

## Run

```
bzfs -loadplugin ./roguehardpoint.so -world mymap.bzw \
     -set _rogueHardPointInterval 300 \
     -set _rogueHardPointTime 60 \
     -set _rogueHardPointWarning 10
```

> **Load only one hardpoint plugin at a time.** `roguehardpoint.so` and the original `hardpoint.so` both register the `HARDPOINT` map object, so they cannot run together on the same server.

## Map object

Add one `hardpoint` object per location to your `.bzw` map. Each one needs a unique name, which is what the server announces.

```
hardpoint
  name red
  pos 0 0 0
  size 30 30 30      # half-extents in x/y, height in z (same as a box)
  rot 45             # optional, degrees counterclockwise (also: rotation)
end

hardpoint
  name blue
  pos 150 -80 0
  size 25 25 30
end
```

| Key | Meaning |
|-----|---------|
| `name` | Name announced to players. Multi-word names are allowed. If omitted, the hardpoint is numbered in the order it appears in the map. |
| `pos` | Position (x y z) of the base of the zone. |
| `size` | Half-extents in x and y, and full height in z, like a box. |
| `rot` / `rotation` | Optional rotation about the z axis, in degrees counterclockwise. |

The zone is invisible to the plugin's rules but you will usually want a matching visual in the map (for example a semi-transparent box or mesh at the same `pos`, `size` and rotation) so players can see its edges.

## Variables

Set these with `-set` on the command line, in a server config, or with `/set` in game as an admin. Defaults are applied automatically if you don't set them.

| Variable | Default | Description |
|----------|---------|-------------|
| `_rogueHardPointInterval` | `300` | Seconds between activations, measured start to start. |
| `_rogueHardPointTime` | `60` | Seconds a hardpoint stays live. |
| `_rogueHardPointWarning` | `10` | Countdown seconds between the announcement and going live. `0` skips the countdown and goes live immediately. |
| `_rogueHardPointFirstDelay` | `30` | Seconds after enough players join until the first hardpoint is announced. |
| `_rogueHardPointMinPlayers` | `2` | Rogue players required on the server for the mode to run. |
| `_rogueHardPointStarSpokes` | `3` | Number of shots in the marker star, spaced evenly in a circle. |
| `_rogueHardPointStarInterval` | `1.0` | Seconds between marker volleys. |

If `_rogueHardPointInterval` is shorter than the countdown plus live time, the next hardpoint starts one second after the previous one ends.

## Scoring

Points are added to the holder's **personal score** (wins). They are separate from kills, and the plugin does not touch team scores.

## Player count

The mode only runs while at least `_rogueHardPointMinPlayers` rogue players are on the server, so a single player cannot farm points alone.

- If there are not enough players, nothing starts.
- When enough players join, the first hardpoint is announced after `_rogueHardPointFirstDelay` seconds.
- If the player count drops below the minimum during a countdown or live hardpoint, it is cancelled with a server message and the timers re-arm once enough players are back.

## Marker star

The sky shots are fired as world weapons (`PZ`) level toward the hardpoint's center from 100 units out and 50 units above it. They are fired in the rogue team color. To make the star look different from player shots, change `eRogueTeam` to another team in `pulseStar()` and rebuild.

## Notes

- Only living (spawned) rogue tanks count toward holding or contesting a hardpoint. Observers and other teams are ignored.
- The zone check uses the tank's position inside the hardpoint's volume, including rotation, so a tank must be within both the footprint and the height range.
- Hardpoints are chosen at random from every `hardpoint` object in the map. With only one hardpoint, it is simply reused each time.
