-- ============================================================================
-- Worked example: an enemy whose combat behaviour is authored entirely in Lua.
--
-- This is the "if this works, almost anything will" proof. Instead of letting
-- the engine run an enemy's original action block (bytecode -> transpiled Lua),
-- we REPLACE one ailist with a hand-written Lua loop that calls engine AI
-- commands directly via ctx:run(opcode, bytes...).
--
-- Every opcode + operand layout used below is taken verbatim from
-- docs/aicommands.md (the generated command reference). Read that alongside
-- docs/luascripting.md (the ctx / pd API).
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

-- byte-encoding helpers (mirror mkshort/mkword in src/include/util.h)
local function u16(v) return (v >> 8) & 0xff, v & 0xff end

-- Opcodes (see docs/aicommands.md). Layout in comments = bytes AFTER the opcode.
local OP_SET_TARGET_CHR  = 0x0106 -- chr(u8) + 0(u8) + 0(u8)
local OP_FACE_ENTITY     = 0x0018 -- attackflags(u16 BE) + entity_id(u16 BE) + label(u8)
local OP_TRY_ATTACK_STAND= 0x0015 -- attackflags(u16 BE) + entity_id(u16 BE) + label(u8)
local OP_IF_LOS_TO_TARGET= 0x003F -- label(u8)   (returns break flag; label ignored here)

-- Constants from src/include/constants.h
local CHR_TARGET = 0xf6
local ATTACKFLAG_AIMONLY    = 0x0020
local ATTACKFLAG_AIMATTARGET= 0x0200

-- per-chr state keyed by chrnum (stable for the chr's lifetime)
local state = {}

-- Engine calls this each frame for any chr whose active ailist is TARGET_AILIST.
local function my_ai(ctx)
  -- 1) Target the player and turn to face them. entity_id 0 = "use target".
  ctx:run(OP_SET_TARGET_CHR, CHR_TARGET, 0, 0)
  ctx:run(OP_FACE_ENTITY, u16(ATTACKFLAG_AIMATTARGET), u16(0), 0)

  -- 2) If we have line of sight, attempt a standing shot. We decide with Lua
  --    (ctx:run returns the command's break flag); the engine's own label-jump
  --    is unused in synthetic mode.
  local los = ctx:run(OP_IF_LOS_TO_TARGET, 0)
  if los ~= 0 then
    local flags = ATTACKFLAG_AIMATTARGET | ATTACKFLAG_AIMONLY  -- 0x0220
    ctx:run(OP_TRY_ATTACK_STAND, u16(flags), u16(0), 0)
  end

  -- 3) Visible heartbeat: a marker + a periodic console log.
  pd.draw_text(8, 200, "LUA-AUTHORED AI ACTIVE", 0x40ff40ff)
  state.t = (state.t or 0) + 1
  if state.t % 120 == 0 then
    pd.log("lua_authored_enemy: tick " .. state.t .. (los ~= 0 and " (LOS, shooting)" or " (no LOS)"))
  end

  -- 4) Yield -- done for this frame. (Never fall through without yielding.)
  return 1
end

pd.register_ailist(TARGET_AILIST, my_ai)
pd.log(string.format("registered Lua-authored AI for ailist 0x%04x", TARGET_AILIST))

-- ============================================================================
-- WHY THIS PROVES THE PIPELINE
--   * The engine asked Lua to run this enemy's action block.
--   * We never touched the original bytecode; we issued engine AI commands
--     (set target, face, line-of-sight check, attack) purely from Lua by
--     opcode + bytes.
--   * Everything the original bytecode could do is reachable the same way --
--     all ~440 commands in docs/aicommands.md are callable via ctx:run.
--
-- Extend it: patrol with run_to_pad (0x0020, pad u16), throw grenades with
-- consider_throwing_grenade (0x001B), play barks, branch on health/alertness,
-- etc. Look each command up in docs/aicommands.md for the exact bytes.
-- ============================================================================
