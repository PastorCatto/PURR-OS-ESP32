-- Verbs: every moy core verb, one screen per group. LEFT/RIGHT switch screens.
--
-- This cart is three things at once: living documentation (each screen's source
-- is a worked example of its verbs), a smoke test for any new implementation
-- (if every screen looks right, you're close), and the seed of the conformance
-- suite (pin golden frames of these screens and "conformance" becomes a diff).
-- Screens 1-7 are core 0.1. Screen 8 is the `layers` STANDARD EXTENSION
-- (declared in this cart's manifest -- a host without it refuses the cart,
-- SPEC.md section 10). Screen 9 exercises SPEC.md section 6.1, which is DRAFT.

local screen = 1
local t = 0                       -- seconds since start (accumulated dt)
local trail = {}                  -- touch-position dots (screen 5)
local vol = 7
local lay                         -- the pre-rendered scroll layer (screen 8)

local function header(name, i, n)
  rect(0, 0, 320, 14, 0)
  print(name, 6, 3, 7)
  print(i .. "/" .. n, 292, 3, 6)
  print("left/right: next screen", 6, 230, 5)
end

-- 1 ------------------------------------------------------------ shapes ------
local function s_shapes()
  cls(1)
  for k = 0, 7 do                               -- line: a swinging fan
    line(60, 120, 60 + flr(50 * math.cos(t + k / 2)),
         120 + flr(50 * math.sin(t + k / 2)), 8 + k % 8)
  end
  rect(130, 60, 60, 40, 3)                      -- rect / rectb
  rectb(126, 56, 68, 48, 11)
  circ(250, 80, 24, 9)                          -- circ / circb
  circb(250, 80, 30, 10)
  for k = 0, 23 do                              -- pix: a dotted ring
    pix(160 + flr(90 * math.cos(t + k / 4)),
        150 + flr(60 * math.sin(t + k / 4)), 7)
  end
  rect(130, 150, 80, 30, 2)
  print("rect", 148, 160, 7)
end

-- 2 ------------------------------------------------------- sprites, map -----
local function s_sprites()
  cls(0)
  map(0, 0, 16, 6, 96, 150)                     -- the cart's tilemap
  print("map()", 150, 128, 6)
  spr(1, 30, 40)                                -- plain tile
  spr(1, 60, 40, -1, 2)                         -- scale 2
  spr(1, 100, 40, -1, 3)                        -- scale 3
  spr(4, 30, 90)                                -- flips
  spr(4, 60, 90, -1, 1, 1)
  spr(4, 90, 90, -1, 1, 2)
  spr(4, 120, 90, -1, 1, 3)
  print("spr scale + flip", 160, 60, 6)
  print("mget(1,1)=" .. mget(1, 1), 160, 90, 6) -- read the map
  mset(1, 1, 3)                                 -- write it (a coin appears)
end

-- 3 ------------------------------------------- camera, clip, pal, palt ------
local function s_state()
  cls(1)
  clip(40 + flr(20 * math.sin(t)), 30, 240, 90) -- everything clips to a window
  camera(flr(6 * math.sin(t * 5)), 0)           -- ...and shakes
  for k = 0, 9 do
    rect(40 + k * 24, 40, 20, 60, k + 2)
  end
  print("inside clip+camera", 90, 70, 7)
  camera()                                      -- reset both
  clip()
  pal(8, 8 + flr(t * 4) % 8)                    -- remap red per frame
  rect(60, 150, 40, 40, 8)
  print("pal(8,..)", 56, 194, 6)
  pal()                                         -- reset
  palt(4, true)                                 -- brick colour 4 -> transparent
  spr(2, 180, 150, -1, 4)
  palt()
  spr(2, 240, 150, -1, 4)
  print("palt vs not", 186, 194, 6)
end

-- 4 --------------------------------------------------- text + palette -------
local function s_text()
  cls(0)
  print("print() in the 8x8 system font", 20, 30, 7)
  for i = 0, 63 do                              -- the whole default palette
    rect(24 + (i % 16) * 17, 70 + flr(i / 16) * 17, 15, 15, i)
  end
  print("the 64 palette entries", 24, 142, 6)
end

-- 5 --------------------------------------------------------------- input ----
local function s_input()
  cls(1)
  local names = { "left", "right", "up", "down", "a", "b", "run" }
  for k, name in ipairs(names) do
    local x = 20 + ((k - 1) % 4) * 72
    local y = 40 + flr((k - 1) / 4) * 34
    rect(x, y, 64, 24, btn(name) and 11 or 5)   -- btn(): held state
    print(name, x + 6, y + 8, btn(name) and 0 or 6)
    if btnp(name) then sfx(0) end               -- btnp(): the press edge
  end
  print("key()=" .. key() .. "  keyp()=" .. keyp(), 20, 120, 6)
  local tx, ty, tap, held = touch()             -- pointer, if the host has one
  if tx then
    if held then trail[#trail + 1] = { tx, ty } end
    if #trail > 40 then table.remove(trail, 1) end
    print("touch " .. tx .. "," .. ty, 20, 134, 6)
  end
  for _, p in ipairs(trail) do pix(p[1], p[2], 10) end
  print("players()=" .. players(), 20, 148, 6)
  print("(btnp anywhere plays sfx 0)", 20, 170, 5)
end

-- 6 --------------------------------------------------------------- audio ----
local function s_audio()
  cls(2)
  print("a: beep()   b: sfx(1)", 20, 40, 7)
  print("up/down: volume(" .. vol .. "/7)", 20, 54, 7)
  print("run: sound_stop()", 20, 68, 7)
  if btnp("a") then beep(330 + 110 * flr(rnd(4)), 0.2) end
  if btnp("b") then sfx(1) end
  if btnp("up") and vol < 7 then vol = vol + 1 volume(vol / 7) end
  if btnp("down") and vol > 0 then vol = vol - 1 volume(vol / 7) end
  if btnp("run") then sound_stop() end
  print("music(n)/music_stop() play sounds.json", 20, 100, 6)
  print("tracks (this cart ships none)", 20, 112, 6)
end

-- 7 -------------------------------------------------------------- system ----
local function s_system()
  cls(0)
  print("time() = " .. time() .. " ms", 20, 40, 7)
  print("rnd()  scatter:", 20, 60, 7)
  for _ = 1, 40 do pix(150 + flr(rnd(140)), 55 + flr(rnd(14)), 10) end
  print("flr(3.7) = " .. flr(3.7), 20, 80, 7)
  print("cfg(\"spin\") = " .. tostring(cfg("spin")), 20, 100, 7)
  print("pmem(0) runs of this cart: " .. pmem(0), 20, 120, 7)
  print("quit() ends a cart; textmode(true)", 20, 150, 5)
  print("switches to typed-text input", 20, 162, 5)
end

-- 8 -------------------------------------------------------------- layers ----
local function s_layer()
  cls(0)
  if not lay then
    lay = make_layer(640, 240)                  -- build the wide world ONCE
    lay:cls(1)
    for k = 0, 30 do
      lay:spr(2, k * 24, 200, -1, 2)            -- a floor of bricks
      lay:spr(3, 40 + k * 40, 120 + (k % 3) * 24)
    end
  end
  draw_layer(lay, 160 + 160 * math.sin(t / 2), 0)   -- window-copy per frame
  print("EXTENSION: layers (SPEC 10)", 20, 18, 9)
  print("make_layer(640,240) built once,", 20, 30, 7)
  print("draw_layer pans it every frame", 20, 42, 7)
end

-- 9 --------------------------------------------------- section 6.1 DRAFT ----
local function s_draft()
  cls(0)
  print("SECTION 6.1 -- DRAFT, not core 0.1", 20, 30, 9)
  tri(60, 160, 120, 60 + flr(20 * math.sin(t)), 180, 160, 12)  -- filled tri
  trib(60, 160, 120, 60 + flr(20 * math.sin(t)), 180, 160, 7)  -- outline
  local w = 24 + flr(20 * math.sin(t * 2))
  sspr(8, 0, 8, 8, 220, 80, w * 2, w * 2)       -- arbitrary stretch of tile 1
  print("tri/trib + sspr stretch", 60, 180, 6)
  print("(spr_batch/rect_batch/spans live", 20, 200, 5)
  print("here too; col_batch is unbuilt)", 20, 212, 5)
end

local screens = {
  { "shapes: pix line rect circ", s_shapes },
  { "sprites + map", s_sprites },
  { "camera clip pal palt", s_state },
  { "text + palette", s_text },
  { "input: btn btnp key touch", s_input },
  { "audio: beep sfx volume", s_audio },
  { "system: time rnd cfg pmem", s_system },
  { "layers: the scroller pattern", s_layer },
  { "6.1 draft: tri sspr", s_draft },
}

function _init()
  pmem(0, pmem(0) + 1)                          -- count runs, persistently
end

function _update(dt)
  t = t + dt
  if btnp("right") then screen = screen % #screens + 1 end
  if btnp("left") then screen = (screen - 2) % #screens + 1 end
end

function _draw()
  screens[screen][2]()
  header(screens[screen][1], screen, #screens)
end
