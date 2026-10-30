# ApexBuffer

Smart adaptive trajectory-based input buffer for Geometry Dash (Geode).  
Designed for both **PC and Android**.

## Current Status (v1.2)

### Working systems
- Input interception + delayed delivery queue
- Stage system (adjustable from Geode settings – mobile friendly)
- Separate Wave stage & horizon
- Auto-Adapt (learns death hotspots and raises/lowers stage)
- Confidence gate + “not worth delaying” checks (reduces delay-then-die)
- Real object gathering (hazards, solids, orbs, pads)
- AABB collision checks against nearby hazards & solids
- Mode-aware simplified physics (Cube, Wave, Ship/UFO)
- On-screen status text
- Practice-only option
- Debug log to file

### Still approximate / planned
- Physics constants are tuned but not 1:1 with the game
- Slope / moving object handling is basic
- Orb interaction (click vs skip) is detected but not fully simulated yet
- Full portal / size / dual / mirror support
- Stronger Ship feathering & Robot / Spider logic

## Settings (all in Geode Mod Settings)

| Setting | Default | Description |
|---------|---------|-------------|
| Enable ApexBuffer | On | Master switch |
| Practice Mode Only | On | Only buffer in practice |
| Stage | 16 | Max delay ticks |
| Wave Stage | 12 | Max delay while wave |
| Aggression | 0.65 | How willing to delay |
| Auto Adapt | On | Adjust stage on hard sections |
| Minimum Confidence | 0.55 | Don't delay below this confidence |
| Lookahead Horizon | 100 | How far ghost looks |
| Wave Horizon | 60 | Wave lookahead |
| Smart Orb Prediction | On | (framework ready) |
| Show Status Text | On | On-screen indicator |
| Debug Log | Off | Write log file |

## Build

```bash
geode build
```

Requires Geode SDK and a matching GD version.

## Philosophy

ApexBuffer prefers **not delaying** when the prediction is uncertain.  
The goal is higher real survival rate, not maximum theoretical delay.
