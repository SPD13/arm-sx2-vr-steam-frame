Per-game profiles: <disc serial>.ini, e.g. SLUS-12345.ini. A copy in the data folder's profiles/
takes precedence over the one shipped here, next to the binary.

Keys:
  lightgun = 1        A light gun game the core does not know as one: light gun mode starts on.
  shot_overlays = 0   Volumetric: 2D overlays centred on a recent shot stay on the glass. On by
                      default: they go to the scene's depth where the shot landed (hit effects drawn
                      as 2D sprites).
  fx_overlays = 1     Volumetric: additive 2D overlays that come and go (effects drawn as 2D
                      sprites anywhere, not only where a shot landed) go to the scene's depth under
                      them. Off by default: a game's HUD could be drawn the same way.
  same_as = SERIAL    This game's profile is SERIAL's (the same game's other regions); the rest of
                      the file is ignored.

Lines can end with a # comment.
