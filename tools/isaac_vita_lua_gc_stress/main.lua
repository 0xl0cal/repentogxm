-- repentogxm Lua GC stress: the stress leg of the LUA-GC.md device gate
-- (review r1 B2).  Test only; never ship it enabled.
--
-- Install: main.lua and metadata.xml to
--   ux0:data/isaacr001/mods/repentogxm_lua_gc_stress/
-- (tools/isaac_vita_sync.py REMOTE_MODS); the lines land in
--   ux0:data/isaacr001/Documents/My Games/Binding of Isaac Repentance/log.txt
-- Remove the folder after the gate.
--
-- From ARM_AFTER game updates after MC_POST_GAME_STARTED, for RUN_FOR game
-- updates (MC_POST_UPDATE runs 30 times a second while unpaused), every
-- update creates PER_UPDATE finalizable LuaBridge userdata and drops them at
-- once: half Isaac.GetPlayer(0) (an EntityPlayer pointer: one
-- lua_newuserdata(8) with the class __gc per call, as EID's FindInRadius /
-- FindByType hits are) and half Vector(i, 0) (a value-type userdata, as EID's
-- Position / __sub results are).  300 per update is ~150 per LuaEngine::Update
-- GC step at 60 steps/s: 2-3x the device's measured EID rate (p99 51, max
-- 71 finalizers per paced step in the GC profile, a lower bound) and above
-- the 70-100 per step at which review-r0's pacing diverged on the host
-- oracle.  At a lower frame rate each GC step sees more.
--
-- One sentinel per update measures the finalizer backlog from inside Lua: a
-- table with a Lua __gc (lgc.c treats it exactly like the userdata: same
-- finobj list, same tobefnz queue), garbage as soon as it is made.  Its
-- __gc only counts and records how many updates it waited (lag).  After
-- RUN_FOR the mod stops creating and keeps reporting for DRAIN updates; the
-- final "done" line must show backlog=0 (the queue drained).
--
-- Lines, via Isaac.DebugString (the game's log.txt), parsed by
-- recomp/vita/lua_gc_window_report.py:
--   [gcstress] armed per_update=300 run_for=18000 drain=900 report_every=300
--   [gcstress] frame=F ud=U sentinels=S finalized=N backlog=S-N lag_max=L
--              lag_mean=M kb=K
--   [gcstress] done frame=... (same fields)
-- frame counts game updates since arming; lag is in game updates; kb is
-- collectgarbage("count") (-1 if the sandbox has no collectgarbage).

local Stress = RegisterMod("repentogxm Lua GC stress", 1)

local PER_UPDATE = 300
local ARM_AFTER = 60        -- 2 s: the start room has rendered
local RUN_FOR = 18000       -- 10 min
local DRAIN = 900           -- 30 s without stress, then "done"
local REPORT_EVERY = 300    -- 10 s

local updates = 0           -- MC_POST_UPDATE calls since load
local armedAt = nil
local finished = false
local ud, sentinels, finalized = 0, 0, 0
local lagMax, lagSum = 0, 0

local sentinelMeta = {
    __gc = function(s)
        finalized = finalized + 1
        local lag = updates - s[1]
        lagSum = lagSum + lag
        if lag > lagMax then
            lagMax = lag
        end
    end
}

local function heapKb()
    if type(collectgarbage) ~= "function" then
        return -1
    end
    local ok, kb = pcall(collectgarbage, "count")
    if not ok or type(kb) ~= "number" then
        return -1
    end
    return math.floor(kb)
end

local function report(prefix, frame)
    local mean = 0
    if finalized > 0 then
        mean = lagSum / finalized
    end
    Isaac.DebugString(string.format(
        "[gcstress] %sframe=%d ud=%d sentinels=%d finalized=%d backlog=%d "
        .. "lag_max=%d lag_mean=%.1f kb=%d",
        prefix, frame, ud, sentinels, finalized, sentinels - finalized,
        lagMax, mean, heapKb()))
end

local function onGameStarted()
    armedAt = updates + ARM_AFTER
    finished = false
    ud, sentinels, finalized, lagMax, lagSum = 0, 0, 0, 0, 0
end

local function onUpdate()
    updates = updates + 1
    if armedAt == nil or finished or updates < armedAt then
        return
    end
    local frame = updates - armedAt
    if frame == 0 then
        Isaac.DebugString(string.format(
            "[gcstress] armed per_update=%d run_for=%d drain=%d report_every=%d",
            PER_UPDATE, RUN_FOR, DRAIN, REPORT_EVERY))
    end
    if frame < RUN_FOR then
        local half = PER_UPDATE // 2
        for i = 1, half do
            local player = Isaac.GetPlayer(0)
            local v = Vector(i, 0)
            player, v = nil, nil
        end
        ud = ud + 2 * half
        setmetatable({ updates }, sentinelMeta)
        sentinels = sentinels + 1
    end
    if frame >= RUN_FOR + DRAIN then
        report("done ", frame)
        finished = true
    elseif frame % REPORT_EVERY == 0 then
        report("", frame)
    end
end

Stress:AddCallback(ModCallbacks.MC_POST_GAME_STARTED, onGameStarted)
Stress:AddCallback(ModCallbacks.MC_POST_UPDATE, onUpdate)
