Audio asset folder. Drop .wav / .flac / .mp3 files here; they are decoded to
mono at the device sample rate at startup (or live with U "rescan" in the F2
dev menu) and become assignable sound sets in dev mode.

Naming:
  thud.wav                      -> set "thud", 1 variant
  thud_01.wav + thud_02.wav     -> set "thud", 2 variants (random per trigger)
  gravel_1.wav ... gravel_8.wav -> set "gravel" (assign to a floor material
                                   for footstep variety)

Names are lowercased and spaces become underscores. A set named like an
existing one (including builtins: water, wind, bird, grass, stone, knock,
thud) appends its files as extra variants of that set.
