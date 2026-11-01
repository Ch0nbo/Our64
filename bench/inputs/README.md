# Bench input scripts

Copy a script to `sd:/wiiu/our64/roms/<rom file name without extension>.input`.
Syntax: `bench/source/input.h`. With `sd:/wiiu/our64/capture-only` present the
bench only captures frames (no speed or compare runs).

`generic.input` mashes Start and A until VI 3000 and captures at four points.
On 2026-10-12, in Cemu, it reached:

| ROM | VI 1200 | VI 2400 | VI 3600 | VI 4800 |
|---|---|---|---|---|
| sm64 | intro letter | castle fly-in | gameplay, castle grounds | gameplay, tutorial sign |
| goldeneye | Dam, intro | Dam, gameplay | watch menu (Start) | watch menu |
| banjo-tooie | title | intro | intro text | cutscene, 3D |
| dkr | logo | race (water) | race (attract) | race |
| fzero-x | race start | race, 509 km/h | race | race |
| oot-mq | intro text | interior, 3D | Deku tree area | grey |
| rogue | Hoth gameplay (attract) | logo | title | title |
| smash | character data | character data | character data | character data |
| jfg | black | menu | menu | menu |

Blast Corps (USA Rev 1) captured nothing in Cemu.
