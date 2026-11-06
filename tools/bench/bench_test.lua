-- Host check of the bench directives (@seed, @room, @remove) of main.lua against bench-save.txt.
-- Run all modes: python bench_run_test.py (needs the lupa package).  One mode by hand:
--   python -c "import lupa.lua53 as L; r=L.LuaRuntime(); r.globals().arg=r.table_from({1:'main.lua',2:'bench-save.txt',3:'set',4:'present'}); r.execute(open('bench_test.lua').read())"
-- @seed sets the start seed in place (Seeds:SetStartSeed) and must never issue the
-- console "seed" command (a --set-stage boot re-rolls the seed of every restart).  Modes: 'set':
-- SetStartSeed takes; 'stuck': it returns but the start seed does not change; 'absent': the
-- binding is missing (calling it raises).  In every mode the list runs once, from one game start.
-- Room API (arg 4, the dry phases' room fields): 'present' = Room:GetBackdropType and Room:HasWater
-- exist (the 1.7.9b bindings), 'absent' = neither exists, 'error' = both raise, 'lookup' = the
-- member lookup itself raises (a userdata __index error), 'huge' = GetBackdropType returns 2^63,
-- an integral float with no integer representation ('%d' raises in Lua 5.3).
-- Room:GetWaterAmount (REPENTOGON only) is a trap in every mode: main.lua must never call it.
local mainPath, savePath, mode, api = arg[1], arg[2], arg[3] or "set", arg[4] or "present"
local f = assert(io.open(savePath, "rb")); local modData = f:read("a"); f:close()

local logLines, cmds, changes = {}, {}, {}
local callbacks = {}
local startSeed = "AAAA AAAA"
local setCalls = {}
local frame = 0

ModCallbacks = { MC_POST_UPDATE = 1, MC_POST_GAME_STARTED = 15, MC_POST_NEW_LEVEL = 18, MC_POST_NEW_ROOM = 19 }
RoomType = { ROOM_DEFAULT = 1, ROOM_BOSS = 5 }
EntityType = {}
Seeds = { Seed2String = function(n) return string.format("S%07d", n) end }

function RegisterMod() local m = {}; function m:AddCallback(id, fn) callbacks[id] = fn end; return m end

-- The floor: Downpour I (the --set-stage boot and "stage 1c") or Basement I ("stage 1").  Downpour
-- rooms have water, Basement rooms none (BackdropType DOWNPOUR = 31, BASEMENT = 1, enums.lua).
local FLOORS = { ["stage 1c"] = { stagetype = 4, name = "Downpour I", backdrop = 31, water = 0.5 },
                 ["stage 1"] = { stagetype = 0, name = "Basement I", backdrop = 1, water = 0.0 } }
local floor = FLOORS["stage 1c"]
local level = { LeaveDoor = 0 }
function level:GetStage() return 1 end
function level:GetStageType() return floor.stagetype end
function level:GetName() return floor.name end
local current = 84 -- the start room; ChangeRoom moves it
function level:GetCurrentRoomIndex() return current end
function level:GetCurrentRoomDesc() return { SafeGridIndex = current } end
function level:GetStartingRoomIndex() return 84 end
local roomData = { {84, 0}, {70, 3}, {85, 9}, {60, 2}, {100, 9}, {71, 5} } -- {grid, spawns}
function level:GetRooms()
    local list = { Size = #roomData + 1 }
    function list:Get(i)
        if i == #roomData then return { SafeGridIndex = 13, Data = { Type = 5, SpawnCount = 50 } } end -- boss: never picked
        return { SafeGridIndex = roomData[i + 1][1], Data = { Type = 1, SpawnCount = roomData[i + 1][2] } }
    end
    return list
end
local room = {}
function room:GetType() return 1 end
local waterAmountCalls = 0
function room:GetWaterAmount() waterAmountCalls = waterAmountCalls + 1; return floor.water end
if api == "present" then
    function room:GetBackdropType() return floor.backdrop end
    function room:HasWater() return floor.water > 0 end
elseif api == "error" then
    function room:GetBackdropType() error("GetBackdropType failed") end
    function room:HasWater() error("HasWater failed") end
elseif api == "lookup" then
    setmetatable(room, { __index = function(_, k)
        if k == "GetBackdropType" or k == "HasWater" then error("no member " .. k) end
    end })
elseif api == "huge" then
    function room:GetBackdropType() return 2 ^ 63 end
    function room:HasWater() return floor.water > 0 end
end
local game = { Difficulty = 0 }
function game:GetLevel() return level end
function game:GetRoom() return room end
function game:IsGreedMode() return false end
function game:GetFrameCount() return frame end
local seeds = {}
function seeds:GetStartSeedString() return startSeed end
if mode ~= "absent" then
    function seeds:SetStartSeed(s)
        setCalls[#setCalls + 1] = s
        if mode == "set" then startSeed = s end
    end
end
function game:GetSeeds() return seeds end
function game:ChangeRoom(idx) changes[#changes + 1] = idx; current = idx end
function Game() return game end

Isaac = {}
function Isaac.DebugString(s) logLines[#logLines + 1] = s end
function Isaac.ConsoleOutput() end
function Isaac.HasModData() return modData ~= nil end
function Isaac.LoadModData() return modData end
function Isaac.RemoveModData() modData = nil end
function Isaac.GetTime() return frame end
function Isaac.ExecuteCommand(c)
    cmds[#cmds + 1] = c
    if FLOORS[c] then -- a new floor, entered in its start room (no ChangeRoom)
        floor = FLOORS[c]
        current = 84
        return "Changed stage."
    end
    return ""
end

dofile(mainPath)
callbacks[ModCallbacks.MC_POST_GAME_STARTED](nil, false)
for _ = 1, 6000 do -- the list waits 4350 updates in all
    frame = frame + 1
    callbacks[ModCallbacks.MC_POST_UPDATE]()
end

local function count(p) local n = 0; for _, c in ipairs(cmds) do if c == p then n = n + 1 end end; return n end
local function logged(p) for _, l in ipairs(logLines) do if l:find(p, 1, true) then return true end end; return false end
for _, c in ipairs(cmds) do assert(not c:match("^seed "), "console seed command issued: " .. c) end
assert(#setCalls == (mode == "absent" and 0 or 1), "SetStartSeed calls " .. #setCalls)
if mode ~= "absent" then assert(setCalls[1] == "S20261017", "SetStartSeed(" .. tostring(setCalls[1]) .. ")") end
if mode == "set" then
    assert(startSeed == "S20261017", "start seed " .. startSeed)
    assert(logged("seed AAAA AAAA -> S20261017: set in place (SetStartSeed) [start]"), "no set-in-place receipt")
    assert(not logged("ERROR seed"), "ERROR seed logged although SetStartSeed took")
else
    assert(logged("ERROR seed: SetStartSeed(S20261017) left AAAA AAAA ("), "no ERROR seed receipt (" .. mode .. ")")
end
-- the stage command must come after the seed: the floor is rebuilt from the new stage seed
local seedAt, stageAt = nil, nil
for i, l in ipairs(logLines) do
    if not seedAt and (l:find("set in place", 1, true) or l:find("ERROR seed", 1, true)) then seedAt = i end
    if not stageAt and l:find("exec [start]: stage 1c", 1, true) then stageAt = i end
end
assert(seedAt and stageAt and seedAt < stageAt, "stage 1c must follow @seed")
assert(count("stage 1c") == 1, "stage once: " .. count("stage 1c"))
assert(count("stage 1") == 1, "dry stage once: " .. count("stage 1"))
assert(count("debug 3") == 1, "debug 3 once (it toggles)")
assert(waterAmountCalls == 0, "Room:GetWaterAmount called " .. waterAmountCalls .. " time(s): absent in 1.7.9b")
assert(count("giveitem c118") == 1, "items once")
assert(count("spawn 27.1") == 4, "spawns once per spawn phase (P3, P8)")
assert(count("spawn 60.1") == 2, "spawns once per spawn phase (P3, P8)")
-- normal rooms by grid (start excluded): 60,70,71,85,100; max spawns -> 85 (tie with 100, lower grid wins).
-- Lead-in @room 1 (60), P1 start, P2 max, P3 2nd, P4 1,3,4,5,6(=1st) then 1st again = the current
-- room -> the next one (70), P5 start, DRY START 3rd (71, the room that ended P5 before the dry
-- section), "stage 1" (Basement start room, no ChangeRoom), lead-in @room 1 (60), P6 start,
-- P7 max, P8 2nd, DONE 3rd.
local want = { 60, 84, 85, 70, 60, 71, 85, 100, 60, 70, 84, 71, 60, 84, 85, 70, 71 }
assert(#changes == #want, "room changes " .. #changes)
for i, v in ipairs(want) do assert(changes[i] == v, "room " .. i .. " = " .. tostring(changes[i])) end
local done, order, fields = false, {}, {}
for _, l in ipairs(logLines) do
    local m = l:match("MARK (%S+ %S+)")
    if m then
        order[#order + 1] = m
        fields[m] = l
    end
    if l:find("MARK BENCH DONE", 1, true) then done = true end
end
assert(done, "BENCH DONE marker")
-- every MARK ends with the room fields of the room it is logged in (the room before the room
-- change that starts its phase); the state part carries stage= and stagetype=
local downpour = { "BENCH START", "P1 start-idle", "P2 fight-maxroom", "P3 spawn-heavy", "P4 transitions",
                   "P5 end-idle", "DRY START" }
local basement = { "P6 dry-start-idle", "P7 dry-fight-maxroom", "P8 dry-spawn-heavy", "BENCH DONE" }
local function wantFields(backdrop, haswater)
    if api == "huge" then return " | backdrop=9.2233720368548e+18 water=? haswater=" .. tostring(haswater) end
    if api ~= "present" then return " | backdrop=? water=? haswater=?" end
    return string.format(" | backdrop=%d water=? haswater=%s", backdrop, tostring(haswater))
end
for _, set in ipairs({ { downpour, 4, 31, true }, { basement, 0, 1, false } }) do
    for _, name in ipairs(set[1]) do
        local l = assert(fields[name], "no MARK " .. name)
        assert(l:find(" stage=1 stagetype=" .. set[2] .. " ", 1, true), "MARK " .. name .. " stage fields: " .. l)
        local tail = wantFields(set[3], set[4])
        assert(l:sub(-#tail) == tail, "MARK " .. name .. " room fields (" .. api .. "): " .. l)
    end
end
-- the dry floor is built after P5's last wait, before P6
local dryAt, stage1At, p6At = nil, nil, nil
for i, l in ipairs(logLines) do
    if not dryAt and l:find("MARK DRY START", 1, true) then dryAt = i end
    if not stage1At and l == "[repro_console] exec [start]: stage 1" then stage1At = i end
    if not p6At and l:find("MARK P6 ", 1, true) then p6At = i end
end
assert(dryAt and stage1At and p6At and dryAt < stage1At and stage1At < p6At, "DRY START, stage 1, P6 out of order")
assert(modData == nil, "@remove left the save data")
local removed, starts = 0, 0
for _, l in ipairs(logLines) do
    if l:find("remove [start]: save data removed, HasModData=false", 1, true) then removed = removed + 1 end
    if l:find("] game start %d+: seed=") then starts = starts + 1 end
end
assert(removed == 1, "remove receipts " .. removed)
assert(starts == 1, "game start lines " .. starts .. " want 1")
assert(table.concat(order, ",") == "BENCH START,P1 start-idle,P2 fight-maxroom,P3 spawn-heavy,P4 transitions,"
    .. "P5 end-idle,DRY START,P6 dry-start-idle,P7 dry-fight-maxroom,P8 dry-spawn-heavy,BENCH DONE",
    table.concat(order, ","))
print("bench_test: PASS (" .. mode .. ", room api " .. api .. ")")
