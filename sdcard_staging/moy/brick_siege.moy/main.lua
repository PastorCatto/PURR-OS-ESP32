-- Brick Siege -- a tank game, and a worked example of moy core 0.1.
--
-- Drive the green tank with the arrows (you MOVE and FACE the way you press) and
-- press A to FIRE -- one of your bullets on screen at a time. Wreck the grey enemy
-- tanks before they reach your EAGLE BASE at the bottom centre. Brick walls crumble
-- cell by cell; steel walls stop bullets dead. Clear the wave to win; lose your
-- lives, or the base, and it is game over (it restarts after a beat).
--
-- WHY THIS CART IS HERE: it uses ONLY core 0.1 -- no extensions, no vendor verbs --
-- so a conformant console must be able to run it as-is. Between them, the twelve
-- verbs it touches are most of what a real game needs:
--
--   drawing    cls  rect  spr  print  map
--   tilemap    mget  mset          (bullets mset bricks away -- the map IS state)
--   input      btn  btnp
--   other      rnd  cfg  sfx
--
-- It declares no `extensions` in its manifest, and nothing here reads a verb from
-- SPEC.md 6.1 (the provisional set). If your implementation runs this file and
-- examples/verbs.moy, it runs core.
--
-- Two things worth copying if you are writing carts:
--   * The whole battlefield is ONE map() call, not a loop over cells. A console is
--     free to make that a single blit; a per-cell loop denies it the chance.
--   * cfg() reads config.json, so the enemy count and attract-mode autoplay are
--     tunable without touching code.
--
-- Reading it as an implementer, two Lua details explain shapes that look odd:
--   * Lua tables are 1-BASED, so records are indexed from 1:
--       player/enemy  p[1..6] = x, y, dir, alive, cooldown, lives (enemy: think)
--       bullet        b[1..4] = x, y, dir, owner
--       boom         bm[1..4] = x, y, life, big
--     and a direction lookup is DV[dir + 1] / P_TANK[dir + 1] / E_TANK[dir + 1].
--   * trunc() below truncates toward zero. A tank nudged off the left edge has a
--     negative x, where truncation and math.floor disagree -- the console's own
--     coordinate handling truncates, so this matches it.
--
-- Copyright (c) Moybyte. MIT, like the rest of this repository.

TS = 16              -- world tile size: each map cell is an 8x8 sheet tile at scale 2
MW = 15              -- tilemap width in cells  (matches map.moymap)
MH = 15              -- tilemap height in cells -> a 240x240 battlefield on the left
FIELD = MW * TS      -- 240: battlefield is the left square; HUD lives in the right strip
BRICK = 8            -- sheet tile id for a destructible brick wall
STEEL = 9            -- sheet tile id for an indestructible steel wall
EAGLE = 10           -- eagle base sprite
BROKEN = 14          -- destroyed-base sprite
BULLET_TILE = 11
EXP_S = 12
EXP_B = 13

-- player/enemy tank sprite tile per facing direction (0=up 1=down 2=left 3=right)
P_TANK = {0, 1, 2, 3}        -- player (green)   -- indexed P_TANK[dir + 1]
E_TANK = {4, 5, 6, 7}        -- enemy (grey/orange)

TANK = 14            -- tank collision box (px) -- a touch under TS so it slips through 1-tile gaps
HALF = TANK // 2
PSPEED = 60.0        -- player tank speed (px/s)
ESPEED = 42.0        -- enemy tank speed (px/s)
BSPEED = 150.0       -- bullet speed (px/s)
COOLDOWN = 0.35      -- min seconds between a tank's shots
LIVES = 3

-- direction -> (dx, dy) unit step   -- indexed DV[dir + 1]
DV = { {0, -1}, {0, 1}, {-1, 0}, {1, 0} }

-- base (eagle) cell, bottom-center of the field
BASE_CX = 7
BASE_CY = 14

-- -- state ------------------------------------------------------------------
players = {}         -- {x, y, dir, alive, cooldown, lives}  (multiplayer hook: list)
enemies = {}         -- {x, y, dir, alive, cooldown, think}  think = retarget timer
bullets = {}         -- {x, y, dir, owner}  owner: 0=player, 1=enemy
booms = {}           -- {x, y, life, big}   explosion particles
spawn_q = 0          -- enemies still waiting to enter the wave
spawn_t = 0.0        -- countdown to the next enemy spawn
base_alive = true
score = 0
state = 0            -- 0 playing, 1 won, 2 game over
state_t = 0.0        -- banner / restart timer
t = 0.0
shake = 0.0

-- Python int(): truncation toward zero. A tank/bullet coordinate goes negative
-- (a tank nudged off the left edge, the screen-shake offset), and there
-- floor(-3.7) = -4 while truncation gives -3, and the console truncates -- so use
-- this, never math.floor.
local function trunc(v)
    if v >= 0 then return math.floor(v) end
    return math.ceil(v)
end


-- -- map helpers ------------------------------------------------------------

function _cell_tile(cx, cy)
    -- tile id in a cell, or -1 if empty / out of the field
    if cx < 0 or cx >= MW or cy < 0 or cy >= MH then
        return STEEL          -- outside the field = a solid wall (keeps tanks in)
    end
    return mget(cx, cy)
end


function _blocks_tank(cx, cy)
    -- any non-empty wall cell stops a tank
    return _cell_tile(cx, cy) >= 0
end


function _tank_hits_wall(x, y)
    -- does a TANKxTANK box at (x, y) overlap any wall cell?
    local cx0 = trunc(x) // TS
    local cy0 = trunc(y) // TS
    local cx1 = trunc(x + TANK - 1) // TS
    local cy1 = trunc(y + TANK - 1) // TS
    local cy = cy0
    while cy <= cy1 do
        local cx = cx0
        while cx <= cx1 do
            if _blocks_tank(cx, cy) then
                return true
            end
            cx = cx + 1
        end
        cy = cy + 1
    end
    return false
end


-- -- spawning ---------------------------------------------------------------

function _make_player(cx)
    -- one player tank, facing up, at cell column cx on the bottom row band.
    return { cx * TS + (TS - TANK) // 2, (MH - 1) * TS + (TS - TANK) // 2,
             0, true, 0.0, LIVES }
end


function _respawn_player(p)
    p[1] = BASE_CX * TS + (TS - TANK) // 2 - 3 * TS
    p[2] = (MH - 1) * TS + (TS - TANK) // 2
    p[3] = 0
    p[4] = true
    p[5] = 0.0
end


-- enemy entry columns (top of the field): left, center, right -- kept clear in the map
ENEMY_COLS = {1, 7, 13}


function _spawn_enemy()
    local cx = ENEMY_COLS[trunc(rnd(#ENEMY_COLS)) + 1]
    local x = cx * TS + (TS - TANK) // 2
    local y = (TS - TANK) // 2
    -- don't stack a new enemy on top of an existing one
    for i = 1, #enemies do
        local e = enemies[i]
        if e[4] and math.abs(e[1] - x) < TANK and math.abs(e[2] - y) < TANK then
            return false
        end
    end
    enemies[#enemies + 1] = { x, y, 1, true, 0.0, 0.0 }   -- facing down, into the field
    return true
end


function _wave_size()
    local n = trunc(tonumber(cfg("enemies", 6)))
    if n < 1 then
        n = 1
    end
    if n > 16 then
        n = 16
    end
    return n
end


function _init()
    -- rebuild the brick/steel field from the cart's tilemap source (map.moymap),
    -- so a finished round starts fresh even though we mset() bricks to empty.
    _reset_field()
    players = { _make_player(BASE_CX - 3) }    -- P1 -- multiplayer: append P2 here
    enemies = {}
    bullets = {}
    booms = {}
    score = 0
    base_alive = true
    state = 0
    state_t = 0.0
    t = 0.0
    shake = 0.0
    spawn_q = _wave_size()
    spawn_t = 0.5
    -- seed a couple of enemies immediately so the field isn't empty on frame 1
    if _spawn_enemy() then
        spawn_q = spawn_q - 1
    end
end


-- The cart's tilemap is shared (mset edits persist for the run), so we snapshot the
-- original layout from map.moymap ONCE and stamp it back at each round start.
_FIELD0 = nil


function _snapshot_field()
    _FIELD0 = {}
    for cy = 0, MH - 1 do
        for cx = 0, MW - 1 do
            _FIELD0[#_FIELD0 + 1] = mget(cx, cy)
        end
    end
end


function _reset_field()
    if _FIELD0 == nil then
        _snapshot_field()
        return
    end
    local i = 1                    -- 1-based: Python's i starts at 0
    for cy = 0, MH - 1 do
        for cx = 0, MW - 1 do
            mset(cx, cy, _FIELD0[i])
            i = i + 1
        end
    end
end


-- -- firing & collisions ----------------------------------------------------

function _fire(tank, owner)
    if tank[5] > 0.0 then
        return false
    end
    -- one bullet at a time per owner side (player) -- count this owner's live shots.
    if owner == 0 then
        for i = 1, #bullets do
            if bullets[i][4] == 0 then
                return false
            end
        end
    end
    local d = tank[3]
    local dv = DV[d + 1]
    -- muzzle at the tank's leading edge, centered on the barrel
    local cx = tank[1] + HALF + dv[1] * HALF
    local cy = tank[2] + HALF + dv[2] * HALF
    bullets[#bullets + 1] = { cx - 2, cy - 2, d, owner }
    tank[5] = COOLDOWN
    sfx(1)
    return true
end


function _boom(x, y, big)
    booms[#booms + 1] = { x, y, big and 0.30 or 0.18, big and 1 or 0 }
    if big then
        shake = 4.0
    end
end


function _hit_tank(bx, by, owner)
    -- a bullet at (bx,by) -- does it hit a tank on the OTHER side? returns true if so.
    -- player bullets (owner 0) hit enemies; enemy bullets hit players.
    if owner == 0 then
        for i = 1, #enemies do
            local e = enemies[i]
            if e[4] and e[1] - 2 <= bx and bx <= e[1] + TANK
                     and e[2] - 2 <= by and by <= e[2] + TANK then
                e[4] = false
                _boom(e[1] + HALF, e[2] + HALF, true)
                score = score + 100
                sfx(2)
                return true
            end
        end
    else
        for i = 1, #players do
            local p = players[i]
            if p[4] and p[1] - 2 <= bx and bx <= p[1] + TANK
                     and p[2] - 2 <= by and by <= p[2] + TANK then
                _kill_player(p)
                return true
            end
        end
    end
    return false
end


function _kill_player(p)
    p[4] = false
    p[6] = p[6] - 1
    _boom(p[1] + HALF, p[2] + HALF, true)
    sfx(2)
end


function _hit_base(bx, by)
    if not base_alive then
        return false
    end
    local x0 = BASE_CX * TS
    local y0 = BASE_CY * TS
    if x0 <= bx and bx <= x0 + TS and y0 <= by and by <= y0 + TS then
        base_alive = false
        _boom(x0 + TS // 2, y0 + TS // 2, true)
        state = 2
        state_t = 1.6
        sfx(2)
        return true
    end
    return false
end


-- -- AI ---------------------------------------------------------------------

function _ai_drive(e, dt)
    -- simple enemy AI: roll forward; on hitting a wall (or now and then at random)
    -- pick a new direction -- biased toward the base so the swarm presses the eagle.
    e[6] = e[6] - dt
    local dv = DV[e[3] + 1]
    local nx = e[1] + dv[1] * ESPEED * dt
    local ny = e[2] + dv[2] * ESPEED * dt
    local stuck = _tank_hits_wall(nx, ny)
    if stuck or e[6] <= 0.0 then
        _ai_retarget(e)
    else
        e[1] = nx
        e[2] = ny
    end
    -- shoot if a wall/target is roughly ahead, or just occasionally
    if e[5] <= 0.0 and rnd(1.0) < 0.012 then
        _fire(e, 1)
    end
end


function _ai_retarget(e)
    e[6] = 0.4 + rnd(1.2)
    -- 60% of the time aim toward the base, else wander
    local bx = BASE_CX * TS
    local by = BASE_CY * TS
    local want
    if rnd(1.0) < 0.6 then
        if math.abs(bx - e[1]) > math.abs(by - e[2]) then
            want = bx > e[1] and 3 or 2
        else
            want = by > e[2] and 1 or 0
        end
    else
        want = trunc(rnd(4))
    end
    -- only accept the new heading if a step that way is clear; else try another
    for _i = 1, 4 do
        local dv = DV[want + 1]
        if not _tank_hits_wall(e[1] + dv[1] * 2, e[2] + dv[2] * 2) then
            e[3] = want
            return
        end
        want = (want + 1) % 4
    end
    e[3] = want
end


function _ai_player(p, dt)
    -- attract auto-pilot: pick the nearest live enemy, LINE UP on its row or column,
    -- then face it and fire. Lining up first (rather than charging diagonally) is what
    -- lets the shots actually connect -- a bullet only travels along an axis.
    local target = nil
    local bestd = 1e9
    for i = 1, #enemies do
        local e = enemies[i]
        if e[4] then
            local d = math.abs(e[1] - p[1]) + math.abs(e[2] - p[2])
            if d < bestd then
                bestd = d
                target = e
            end
        end
    end
    if target == nil then
        return 0, 0, false
    end
    local ex = target[1]
    local ey = target[2]
    local dxp = ex - p[1]
    local dyp = ey - p[2]
    local aligned_col = math.abs(dxp) < 8      -- same vertical lane -> can shoot up/down
    local aligned_row = math.abs(dyp) < 8      -- same horizontal lane -> can shoot left/right
    local ddx = 0
    local ddy = 0
    local fire = false
    if aligned_col then
        -- lined up vertically: hold still, FACE the enemy, and shoot. Returning no
        -- movement keeps _move_tank from spinning the turret off-target this frame.
        p[3] = dyp > 0 and 1 or 0
        fire = true
    elseif aligned_row then
        p[3] = dxp > 0 and 3 or 2
        fire = true
    else
        -- not lined up: drive to close the SMALLER gap first (so it snaps onto a
        -- shared row/column quickly), then the other branch takes the shot.
        if math.abs(dxp) <= math.abs(dyp) then
            ddx = dxp > 0 and 1 or -1
        else
            ddy = dyp > 0 and 1 or -1
        end
    end
    return ddx, ddy, fire
end


-- -- per-frame update -------------------------------------------------------

function _move_tank(tank, ddx, ddy, speed, dt)
    -- set facing from the input, then step on whichever axis was pressed, sliding
    -- along walls (try X then Y) so the tank doesn't jam on a corner.
    if ddx > 0 then
        tank[3] = 3
    elseif ddx < 0 then
        tank[3] = 2
    elseif ddy > 0 then
        tank[3] = 1
    elseif ddy < 0 then
        tank[3] = 0
    end
    if ddx ~= 0 then
        local nx = tank[1] + ddx * speed * dt
        if not _tank_hits_wall(nx, tank[2]) then
            tank[1] = nx
        end
    end
    if ddy ~= 0 then
        local ny = tank[2] + ddy * speed * dt
        if not _tank_hits_wall(tank[1], ny) then
            tank[2] = ny
        end
    end
    -- keep tanks inside the battlefield square
    if tank[1] < 0 then
        tank[1] = 0
    end
    if tank[1] > FIELD - TANK then
        tank[1] = FIELD - TANK
    end
    if tank[2] < 0 then
        tank[2] = 0
    end
    if tank[2] > FIELD - TANK then
        tank[2] = FIELD - TANK
    end
end


function _update(dt)
    t = t + dt
    if shake > 0.0 then
        shake = math.max(0.0, shake - dt * 14.0)
    end
    -- explosions always tick (so the boom animates through a banner)
    local keep = {}
    for i = 1, #booms do
        local bm = booms[i]
        bm[3] = bm[3] - dt
        if bm[3] > 0.0 then
            keep[#keep + 1] = bm
        end
    end
    booms = keep                   -- Python's booms[:] = keep (nothing else aliases it)

    if state ~= 0 then
        state_t = state_t - dt
        if state_t <= 0.0 then
            _init()
        end
        return
    end

    local auto = cfg("autoplay", 0)

    -- players (the loop is the multiplayer hook -- add P2 to `players` and it just works)
    for i = 1, #players do
        local p = players[i]
        if not p[4] then
            goto continue
        end
        if p[5] > 0.0 then
            p[5] = math.max(0.0, p[5] - dt)
        end
        local ddx = 0
        local ddy = 0
        local fire = false
        -- P1 reads the one hardware pad. (P2 would read a 2nd pad here.)
        if p == players[1] then
            local left = btn("left")
            local right = btn("right")
            local up = btn("up")
            local down = btn("down")
            local any_in = left or right or up or down or btnp("a")
            if auto ~= 0 and not any_in then       -- 0 is TRUTHY in Lua: compare it
                ddx, ddy, fire = _ai_player(p, dt)
            else
                if left then
                    ddx = -1
                elseif right then
                    ddx = 1
                elseif up then
                    ddy = -1
                elseif down then
                    ddy = 1
                end
                fire = btnp("a")
            end
        end
        if ddx ~= 0 or ddy ~= 0 then
            _move_tank(p, ddx, ddy, PSPEED, dt)
        end
        if fire then
            _fire(p, 0)
        end
        ::continue::
    end

    -- all players dead but lives remain -> respawn the dead ones; none left -> over
    local living = 0
    for i = 1, #players do
        local p = players[i]
        if p[4] then
            living = living + 1
        elseif p[6] > 0 then
            _respawn_player(p)
            living = living + 1
        end
    end
    if living == 0 then
        state = 2
        state_t = 1.6
        return
    end

    -- enemies
    for i = 1, #enemies do
        local e = enemies[i]
        if not e[4] then
            goto continue
        end
        if e[5] > 0.0 then
            e[5] = math.max(0.0, e[5] - dt)
        end
        _ai_drive(e, dt)
        ::continue::
    end

    -- feed the wave: spawn the queued enemies a few at a time
    if spawn_q > 0 and _alive_enemies() < 4 then
        spawn_t = spawn_t - dt
        if spawn_t <= 0.0 then
            if _spawn_enemy() then
                spawn_q = spawn_q - 1
            end
            spawn_t = 1.4
        end
    end

    -- bullets
    local bk = {}
    for i = 1, #bullets do
        local b = bullets[i]
        local dv = DV[b[3] + 1]
        b[1] = b[1] + dv[1] * BSPEED * dt
        b[2] = b[2] + dv[2] * BSPEED * dt
        local bx = b[1] + 2
        local by = b[2] + 2
        -- off the battlefield?
        if bx < 0 or bx > FIELD or by < 0 or by > FIELD then
            goto continue
        end
        -- wall?
        local cx = trunc(bx) // TS
        local cy = trunc(by) // TS
        local tile = _cell_tile(cx, cy)
        if tile == BRICK then
            mset(cx, cy, -1)          -- crumble the brick cell
            _boom(cx * TS + TS // 2, cy * TS + TS // 2, false)
            sfx(0)
            goto continue
        end
        if tile == STEEL then
            _boom(bx, by, false)
            sfx(0)
            goto continue
        end
        -- base?
        if _hit_base(bx, by) then
            goto continue
        end
        -- tank on the other side?
        if _hit_tank(bx, by, b[4]) then
            goto continue
        end
        bk[#bk + 1] = b
        ::continue::
    end
    bullets = bk                   -- Python's bullets[:] = bk

    -- win? all queued spawned AND none left alive
    if spawn_q == 0 and _alive_enemies() == 0 then
        state = 1
        state_t = 2.0
        sfx(2)
    end
end


function _alive_enemies()
    local n = 0
    for i = 1, #enemies do
        if enemies[i][4] then
            n = n + 1
        end
    end
    return n
end


-- -- draw -------------------------------------------------------------------

function _draw()
    local sx = 0
    local sy = 0
    if shake > 0.0 then
        sx = trunc(rnd(shake * 2) - shake)
        sy = trunc(rnd(shake * 2) - shake)
    end
    -- The battlefield backdrop. The Python twin DECLARES it once via background()
    -- and lets the engine repaint it each frame, but that verb is the `layers`
    -- standard extension and this cart is deliberately moy core 0.1 ONLY, so it
    -- clears explicitly. The draw stream is IDENTICAL either way: background()'s
    -- restore hook is itself a cls(), fired at this same point in the frame.
    -- (The HUD strip below repaints its own black over it.)
    cls(1)                                 -- 1 = dark_blue
    -- the whole brick/steel field in ONE native map() call (#32): 15x15 cells of
    -- 8x8 tiles at scale 2 -> 16px world blocks. Destroyed bricks are empty cells.
    map(0, 0, MW, MH, sx, sy, 0, 2)

    -- Every moving sprite (eagle + enemies + players + bullets + explosions) is one
    -- spr() at colorkey 0, scale 2. A plain contiguous run of spr() calls like this
    -- is the idiom core 0.1 asks for (SPEC.md 7.1: "many tiles is a plain loop over
    -- spr"), and it leaves a console free to coalesce the run into one blit.
    -- the eagle base (or its rubble) at the fortress center
    local bx = BASE_CX * TS + sx
    local by = BASE_CY * TS + sy
    spr(base_alive and EAGLE or BROKEN, bx, by, 0, 2)
    -- enemies
    for i = 1, #enemies do
        local e = enemies[i]
        if e[4] then
            spr(E_TANK[e[3] + 1], trunc(e[1]) + sx, trunc(e[2]) + sy, 0, 2)
        end
    end
    -- players
    for i = 1, #players do
        local p = players[i]
        if p[4] then
            spr(P_TANK[p[3] + 1], trunc(p[1]) + sx, trunc(p[2]) + sy, 0, 2)
        end
    end
    -- bullets
    for i = 1, #bullets do
        local b = bullets[i]
        spr(BULLET_TILE, trunc(b[1]) + sx - 4, trunc(b[2]) + sy - 4, 0, 2)
    end
    -- explosions
    for i = 1, #booms do
        local bm = booms[i]
        spr(bm[4] ~= 0 and EXP_B or EXP_S, trunc(bm[1]) - 8 + sx,
            trunc(bm[2]) - 8 + sy, 0, 2)
    end

    -- -- HUD (right strip) --
    local hx = FIELD + 4
    rect(FIELD, 0, W - FIELD, H, 0)
    print("BATTLE", hx, 6, 10)
    print("CITY", hx, 16, 10)
    print("SCORE", hx, 36, 6)
    print(tostring(score), hx, 46, 7)
    print("LEFT", hx, 64, 6)
    print(tostring(spawn_q + _alive_enemies()), hx, 74, 7)
    print("LIVES", hx, 92, 6)
    local p0 = nil
    if #players > 0 then
        p0 = players[1]
    end
    local lv = 0
    if p0 ~= nil then
        lv = p0[6]
    end
    -- draw a little tank icon per life
    local i = 0
    while i < lv and i < 5 do
        spr(P_TANK[1], hx + i % 2 * 18, 104 + (i // 2) * 14, 0, 1)
        i = i + 1
    end
    -- base status pip
    print("BASE", hx, 150, 6)
    rect(hx, 160, 10, 8, base_alive and 11 or 8)

    -- banners
    if state == 1 then
        print("WAVE CLEAR!", FIELD // 2 - 44, FIELD // 2 - 4, 10)
    elseif state == 2 then
        print("GAME OVER", FIELD // 2 - 36, FIELD // 2 - 4, 8)
    end
end
