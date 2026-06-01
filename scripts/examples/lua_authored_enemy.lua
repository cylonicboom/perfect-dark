-- ============================================================================
-- Worked example: an enemy whose combat behaviour is authored entirely in Lua.
--
-- This is the "if this works, almost anything will" proof. Instead of letting
-- the engine run an enemy's original action block (bytecode -> transpiled Lua),
-- we REPLACE one ailist with a hand-written Lua loop that:
--   * uses the generated helper library scripts/ai.lua (ai.<command>(ctx, ...))
--     instead of raw ctx:run(opcode, bytes...), and
--   * reads ctx:self() to make per-enemy decisions (this chr's health, target,
--     position) and keep per-chr memory.
--
-- Command names/operands come from docs/aicommands.md (the generated reference);
-- the ctx / pd / ctx:self API is in docs/luascripting.md.
--
-- HOW TO USE
--   1. Start a level, open the ~ console, watch the AI X-ray overlay, and note
--      an ailist id an enemy is actually running (e.g. 0x0021).
--   2. Set TARGET_AILIST below to that id.
--   3. Put this logic in scripts/init.lua (or dofile it), then /lua reload.
--   4. That enemy now runs YOUR Lua instead of its original list.
--
-- CONTRACT (identical to native action blocks):
--   * Return 1 to YIELD (done this frame), 0 if the list finished, 2 if you
--     switched the active list. You MUST yield once per frame or you soft-lock.
--   * Control flow is plain Lua. There is no bytecode to jump into, so the
--     trailing `label` byte that goto/if_*/try_* commands take is IGNORED in
--     synthetic mode -- pass 0 for it and branch with Lua instead.
--   * Multi-byte operands are BIG-ENDIAN (matching mkshort/mkword): a u16 v is
--     (v>>8)&0xff, v&0xff ; a u32 is its 4 BE bytes.
-- ============================================================================

local TARGET_AILIST = 0x0021  -- <-- set to an id you saw in the AI X-ray

-- The generated helper library turns raw opcode/byte calls into named
-- functions (one per command in docs/aicommands.md). Compare:
--   ctx:run(0x0015, u16(0x220), u16(0), 0)   -- raw
--   ai.try_attack_stand(ctx, 0x220, 0, 0)    -- with ai.lua
local ai = dofile("scripts/ai.lua")

-- Constants from src/include/constants.h
local CHR_TARGET = 0xf6
local ATTACKFLAG_AIMONLY     = 0x0020
local ATTACKFLAG_AIMATTARGET = 0x0200

-- per-chr state keyed by chrnum (stable for the chr's lifetime)
-- Per-chr memory, keyed by chrnum so each enemy running this list has its own
-- state (ctx:self().chrnum is stable for the chr's lifetime).
local state = {}

-- Engine calls this each frame for any chr whose active ailist is TARGET_AILIST.
local function my_ai(ctx)
  -- 0) Who am I? ctx:self() tells us this specific chr's state so we can make
  --    per-enemy decisions and keep per-enemy memory.
  local me = ctx:self()
  if not me then return 1 end           -- object-driven list, nothing to do
  local mem = state[me.chrnum]
  if not mem then mem = { ticks = 0 }; state[me.chrnum] = mem end
  mem.ticks = mem.ticks + 1

  -- 1) Target the player and turn to face them. entity_id 0 = "use target".
  ai.set_target_chr(ctx, CHR_TARGET)
  ai.try_face_entity(ctx, ATTACKFLAG_AIMATTARGET, 0, 0)

  -- 2) Decide what to do based on MY health (real per-enemy behaviour). When
  --    badly hurt, dive sideways instead of standing still to shoot.
  local hurt = me.maxhealth > 0 and (me.health / me.maxhealth) < 0.35
  local los = ai.if_los_to_target(ctx, 0)

  if los ~= 0 and not hurt then
    local flags = ATTACKFLAG_AIMATTARGET | ATTACKFLAG_AIMONLY  -- 0x0220
    ai.try_attack_stand(ctx, flags, 0, 0)
  elseif hurt then
    ai.try_sidestep(ctx, 0)             -- evade while low
  end

  -- 3) Per-enemy HUD label above-ish, plus a periodic per-chr log.
  pd.draw_text(8, 200, string.format("LUA AI: chr %d  hp %d/%d  %s",
      me.chrnum, math.floor(me.health), math.floor(me.maxhealth),
      hurt and "EVADING" or (los ~= 0 and "SHOOTING" or "SEEKING")),
      hurt and 0xff4040ff or 0x40ff40ff)
  if mem.ticks % 120 == 0 then
    pd.log(string.format("chr %d: tick %d hp=%.0f alert=%d",
        me.chrnum, mem.ticks, me.health, me.alertness))
  end

  -- 4) Yield -- done for this frame. (Never fall through without yielding.)
  return 1
end

pd.register_ailist(TARGET_AILIST, my_ai)
pd.log(string.format(
  "lua_authored_enemy: registered for ailist 0x%04x -- set TARGET_AILIST to an "
  .. "id from the AI X-ray, then /lua reload", TARGET_AILIST))

-- ============================================================================
-- WHY THIS PROVES THE PIPELINE
--   * The engine asked Lua to run this enemy's action block.
--   * We never touched the original bytecode; we issued engine AI commands
--     (set target, face, line-of-sight check, attack) purely from Lua by
--     opcode + bytes.
--   * Everything the original bytecode could do is reachable the same way --
--     all ~440 commands in docs/aicommands.md are callable via ctx:run.
--
-- Extend it: patrol with ai.run_to_pad(ctx, pad), throw grenades with
-- ai.consider_throwing_grenade(ctx, ...), play barks, branch on health /
-- alertness, etc. Every command is in ai.lua; see docs/aicommands.md for what
-- each one does and its exact operands.
-- ============================================================================
