-- repro_console: run debug-console commands on the PS Vita without a keyboard.
--
-- The command list lives in this mod's save data, i.e. the plain-text file
--   <isaac data root>/data/repro_console/save<slot>.dat
-- (on the Vita: ux0:data/isaacr001/data/repro_console/save2.dat for save slot 2),
-- one command per line.  Edit it over FTP; nothing in this file needs to change.
--
-- File format:
--   once                  first line: delete the file after the start commands ran
--   @continue             also run the start commands when a run is *continued*
--   # comment             ignored, as are empty lines
--   <command>             run at MC_POST_GAME_STARTED (new run; continued only with @continue)
--   @newlevel <command>   run at every MC_POST_NEW_LEVEL after the run started
--   @bossroom <command>   run once per floor, on entering the first boss-type room
--   @sweep <cmd> <first> <last> [<next>]
--                         one id per new run: run "<cmd> <n>" (n = <next>, default <first>),
--                         rewrite this line with n+1 (Isaac.SaveModData) so the next new run
--                         plays the next id, delete the file after <last>.  Gated like the
--                         start commands on Continue.  E.g. "@sweep cutscene 3 25".
--   @marker <text>        pseudo-command (any list): log "MARK <text> | <state> | <entity census> |
--                         backdrop=<n> water=? haswater=<bool>" (state has stage= and stagetype=;
--                         room fields: see roomFields, '?' = binding absent or failing)
--   @wait <n>             pseudo-command (any list): run the rest of that list n game updates
--                         later (MC_POST_UPDATE, 30/s while unpaused), e.g. spawns 1 s after
--                         the floor changed.  'once' still removes the file immediately.
--   @census <n>           pseudo-command: log "CENSUS <state> <entity census>" every n game
--                         updates (0 = off) - the load level of a benchmark room over time.
--   @seed <n>             pseudo-command (bench): set the run's start seed to
--                         Seeds.Seed2String(n) in place with Seeds:SetStartSeed - no restart.  A
--                         --set-stage boot starts every run, a "seed X" restart included, through
--                         Game::StartDebug, which rolls a random seed, so the console "seed" command
--                         cannot seed it.  SetStartSeed recomputes the stage seeds; the floor is only
--                         rebuilt from them by a "stage <x>" command, so put one right after @seed
--                         (log.txt then shows "Level::Init ... Seed <stage seed>", which
--                         bench_run.py checks against the seed).
--   @room <sel>           pseudo-command (bench): Game():ChangeRoom to a room of the current floor;
--                         sel = start | max (normal room with the most spawns) | <k> (k-th normal
--                         room by grid index, wrapping).  Deterministic for a fixed seed.
--   @remove               pseudo-command (bench): Isaac.RemoveModData now, so a later start of
--                         this save file does not replay the list (the bench's last line).
--
-- Every MC_POST_GAME_STARTED logs "game start <n>: seed=<start seed> continued=<bool>", so a
-- restart (or its absence) is visible in log.txt without any other probe.
-- Everything the mod does is written to the game's log.txt through Isaac.DebugString
-- with the prefix "[repro_console]".  No require(): the port's require ignores package.path.

local mod = RegisterMod("repro_console", 1)
local TAG = "[repro_console] "

local spec = nil          -- parsed command list of the current run (nil = nothing loaded)
local bossFloorsDone = {} -- "<stage>:<stagetype>" -> true once @bossroom commands ran there
local queue = {}          -- {cmd=, why=} entries deferred by "@wait <n>", run from MC_POST_UPDATE
local waitUpdates = 0     -- game updates left before the queue runs
local censusEvery = 0     -- "@census <n>": log an entity census every n game updates (0 = off)
local censusCount = 0

-- Entity type ids (EntityType enum of the Repentance API; numeric fallbacks for the harness)
local ET = EntityType or {}
local T_PLAYER = ET.ENTITY_PLAYER or 1
local T_TEAR = ET.ENTITY_TEAR or 2
local T_FAMILIAR = ET.ENTITY_FAMILIAR or 3
local T_LASER = ET.ENTITY_LASER or 7
local T_PROJECTILE = ET.ENTITY_PROJECTILE or 9
local T_EFFECT = ET.ENTITY_EFFECT or 1000

local function log(msg)
    Isaac.DebugString(TAG .. msg)
    Isaac.ConsoleOutput(TAG .. msg .. "\n")
end

local function oneLine(s)
    s = tostring(s)
    s = s:gsub("\r", "")
    s = s:gsub("\n+$", "")
    s = s:gsub("\n", " | ")
    return s
end

-- "@sweep <cmd> <first> <last> [<next>]" -> table or nil.  <cmd> is one word (the console
-- command the id is appended to), <next> is the id to play on this run (written by the mod).
local function parseSweep(rest, lineNo)
    local cmd, first, last, nxt = rest:match("^(%S+)%s+(%d+)%s+(%d+)%s*(%d*)$")
    if not cmd then
        return nil
    end
    first, last = tonumber(first), tonumber(last)
    if first > last then
        return nil
    end
    nxt = tonumber(nxt) or first
    return { cmd = cmd, first = first, last = last, next = nxt, line = lineNo }
end

local function parse(raw)
    local out = { start = {}, newlevel = {}, bossroom = {}, once = false, continued = false, n = 0,
                  sweep = nil, lines = {} }
    raw = raw:gsub("^\239\187\191", "") -- UTF-8 BOM from Windows editors
    for rawLine in (raw .. "\n"):gmatch("([^\n]*)\n") do
        local line = rawLine:gsub("\r", "")
        line = line:match("^%s*(.-)%s*$")
        table.insert(out.lines, line) -- kept verbatim (minus CR/BOM) so @sweep can rewrite the file
        if line ~= "" and line:sub(1, 1) ~= "#" then
            local lower = line:lower()
            if lower == "once" then
                out.once = true
            elseif lower == "@continue" then
                out.continued = true
            elseif lower == "@remove" then
                table.insert(out.start, "@remove") -- pseudo-command, see exec()
                out.n = out.n + 1
            else
                local kind, cmd = line:match("^@(%w+)%s+(.+)$")
                if kind then
                    kind = kind:lower()
                    if kind == "sweep" then
                        local sw = parseSweep(cmd, #out.lines)
                        if not sw then
                            log("bad sweep directive ignored (want '@sweep <cmd> <first> <last> [<next>]'): " .. line)
                        elseif out.sweep then
                            log("second sweep directive ignored: " .. line)
                        else
                            out.sweep = sw
                            out.n = out.n + 1
                        end
                    elseif kind == "marker" or kind == "wait" or kind == "census" or kind == "seed"
                            or kind == "room" then
                        table.insert(out.start, line) -- pseudo-commands, see exec() / runList()
                        out.n = out.n + 1
                    elseif type(out[kind]) == "table" then
                        table.insert(out[kind], cmd)
                        out.n = out.n + 1
                    else
                        log("unknown directive ignored: " .. line)
                    end
                else
                    table.insert(out.start, line)
                    out.n = out.n + 1
                end
            end
        end
    end
    -- the gmatch above yields one extra empty element for the trailing "\n" we appended
    if out.lines[#out.lines] == "" then
        table.remove(out.lines)
    end
    return out
end

local function whereAmI()
    local game = Game()
    local level = game:GetLevel()
    local room = game:GetRoom()
    return string.format(
        "greed=%s difficulty=%d stage=%d stagetype=%d level=%s roomtype=%d roomidx=%d frame=%d",
        tostring(game:IsGreedMode()), game.Difficulty, level:GetStage(), level:GetStageType(),
        tostring(level:GetName()), room:GetType(), level:GetCurrentRoomIndex(), game:GetFrameCount())
end

-- Entity census of the current room: total and per class (NPC = EntityType 10..999).
local function census()
    if type(Isaac.GetRoomEntities) ~= "function" then
        return "entities=n/a"
    end
    local ok, list = pcall(Isaac.GetRoomEntities)
    if not ok or type(list) ~= "table" then
        return "entities=n/a"
    end
    local c = { npc = 0, tears = 0, familiars = 0, projectiles = 0, effects = 0, lasers = 0, other = 0 }
    for _, e in ipairs(list) do
        local t = e.Type
        if t == T_TEAR then
            c.tears = c.tears + 1
        elseif t == T_FAMILIAR then
            c.familiars = c.familiars + 1
        elseif t == T_PROJECTILE then
            c.projectiles = c.projectiles + 1
        elseif t == T_EFFECT then
            c.effects = c.effects + 1
        elseif t == T_LASER then
            c.lasers = c.lasers + 1
        elseif type(t) == "number" and t >= 10 and t < T_EFFECT then
            c.npc = c.npc + 1
        elseif t ~= T_PLAYER then
            c.other = c.other + 1
        end
    end
    return string.format("entities=%d npc=%d tears=%d familiars=%d projectiles=%d effects=%d lasers=%d other=%d",
        #list, c.npc, c.tears, c.familiars, c.projectiles, c.effects, c.lasers, c.other)
end

-- Room fields of every MARK line (the bench's dry phases): the backdrop and the water of the room
-- the marker is logged in, so a phase's room is proven dry or not from log.txt alone.  Both
-- bindings are registered in the Room class of the frozen 1.7.9b PE (push fn, push name):
--   Room:GetBackdropType = 0x4baea0  mov eax,[ecx+0x1e84]; ret
--   Room:HasWater        = 0x7db890  water amount [ecx+0x7134] > 0.0f, the same field and test
--                                    that gate the room's water pass 0x7dbfe0
-- The water amount itself (Room:GetWaterAmount) is a REPENTOGON addition and not in 1.7.9b (the
-- PE holds no such name): it is never called and logs as water=?.  A binding that is missing or
-- fails logs '?' for its field (pcall), the marker line itself never fails.
local function roomFields()
    local okRoom, room = pcall(function() return Game():GetRoom() end)
    local function field(name)
        if not okRoom or room == nil then
            return "?"
        end
        local okFn, fn = pcall(function() return room[name] end)
        if not okFn or type(fn) ~= "function" then
            return "?"
        end
        local ok, v = pcall(fn, room)
        if not ok or v == nil then
            return "?"
        end
        if type(v) == "number" and v == v and v ~= math.huge and v ~= -math.huge and v == math.floor(v) then
            -- an integral float outside the integer range (2^63) makes %d raise in Lua 5.3:
            -- fall back to tostring rather than lose the marker line
            local okFmt, s = pcall(string.format, "%d", v)
            if okFmt then
                return s
            end
        end
        return tostring(v)
    end
    return string.format("backdrop=%s water=? haswater=%s", field("GetBackdropType"), field("HasWater"))
end

local function gameTime()
    if type(Isaac.GetTime) ~= "function" then
        return "n/a"
    end
    local ok, t = pcall(Isaac.GetTime)
    return ok and tostring(t) or "n/a"
end

local gameStarts = 0   -- MC_POST_GAME_STARTED count (same Lua state across restarts)

-- "@room <sel>" -> grid index or nil, see the header.
local function pickRoom(sel)
    local level = Game():GetLevel()
    local start = level:GetStartingRoomIndex()
    if sel == "start" then
        return start
    end
    local rooms = level:GetRooms()
    local list = {}
    for i = 0, rooms.Size - 1 do
        local d = rooms:Get(i)
        if d and d.Data and d.Data.Type == RoomType.ROOM_DEFAULT and d.SafeGridIndex ~= start then
            list[#list + 1] = { idx = d.SafeGridIndex, spawns = d.Data.SpawnCount or 0 }
        end
    end
    if #list == 0 then
        return nil
    end
    table.sort(list, function(a, b) return a.idx < b.idx end)
    if sel == "max" then
        local best = list[1]
        for _, r in ipairs(list) do
            if r.spawns > best.spawns then
                best = r
            end
        end
        return best.idx
    end
    local k = tonumber(sel)
    if not k then
        return nil
    end
    -- A phase must start with a real room change: when the k-th room is the current one, take
    -- the next one (still deterministic for a fixed seed and script).
    local i = ((k - 1) % #list) + 1
    local okCur, cur = pcall(function()
        local desc = level.GetCurrentRoomDesc and level:GetCurrentRoomDesc()
        if desc and desc.SafeGridIndex then
            return desc.SafeGridIndex
        end
        return level:GetCurrentRoomIndex()
    end)
    if okCur and list[i].idx == cur and #list > 1 then
        i = (i % #list) + 1
    end
    return list[i].idx
end

local function exec(cmd, why)
    local seedN = cmd:match("^@seed%s+(%d+)$")
    if seedN then
        -- In place, never "seed X": see the header (@seed).
        local want = Seeds.Seed2String(tonumber(seedN))
        local seeds = Game():GetSeeds()
        local have = seeds:GetStartSeedString()
        if have == want then
            log(string.format("seed %s active (already the start seed) [%s]", have, why))
            return nil
        end
        local ok, err = pcall(function() seeds:SetStartSeed(want) end)
        local now = seeds:GetStartSeedString()
        if ok and now == want then
            log(string.format("seed %s -> %s: set in place (SetStartSeed) [%s]; the next stage command "
                .. "builds the floor from it", have, want, why))
        else
            log(string.format("ERROR seed: SetStartSeed(%s) left %s (%s) - continuing unseeded", want,
                tostring(now), ok and "no effect" or tostring(err)))
        end
        return nil
    end
    local sel = cmd:match("^@room%s+(%S+)$")
    if sel then
        local idx = pickRoom(sel)
        if not idx then
            log(string.format("ERROR room %s: no such room", sel))
            return nil
        end
        log(string.format("room %s -> grid %d [%s]", sel, idx, why))
        local level = Game():GetLevel()
        level.LeaveDoor = -1
        Game():ChangeRoom(idx)
        return nil
    end
    if cmd == "@remove" then
        Isaac.RemoveModData(mod)
        log(string.format("remove [%s]: save data removed, HasModData=%s", why, tostring(Isaac.HasModData(mod))))
        return nil
    end
    local text = cmd:match("^@marker%s+(.+)$")
    if text then
        log(string.format("MARK %s | %s time=%s | %s | %s", text, whereAmI(), gameTime(), census(), roomFields()))
        return
    end
    local every = cmd:match("^@census%s+(%d+)$")
    if every then
        censusEvery = tonumber(every)
        censusCount = 0
        if censusEvery > 0 then
            log(string.format("census every %d update(s) [%s]: %s", censusEvery, why, census()))
        else
            log(string.format("census off [%s]", why))
        end
        return
    end
    log(string.format("exec [%s]: %s", why, cmd))
    local ok, ret = pcall(Isaac.ExecuteCommand, cmd)
    if ok then
        log(string.format("result [%s]: %s", cmd, oneLine(ret == nil and "" or ret)))
    else
        log(string.format("ERROR [%s]: %s", cmd, tostring(ret)))
    end
end

-- Run a command list in order; "@wait <n>" defers the rest of the list (and anything already
-- pending) by n game updates, served from MC_POST_UPDATE.  n = 0 counts as 1.
local function runList(cmds, why)
    for i, cmd in ipairs(cmds) do
        local n = cmd:match("^@wait%s+(%d+)$")
        if n then
            local rest = {}
            for j = i + 1, #cmds do
                rest[#rest + 1] = { cmd = cmds[j], why = why }
            end
            for _, e in ipairs(queue) do
                rest[#rest + 1] = e
            end
            queue = rest
            waitUpdates = math.max(tonumber(n), 1)
            log(string.format("wait %d update(s) [%s]: %d command(s) deferred", waitUpdates, why, #rest))
            return
        end
        exec(cmd, why)
    end
end

local function onUpdate()
    if waitUpdates > 0 then
        waitUpdates = waitUpdates - 1
        if waitUpdates == 0 and #queue > 0 then
            local pending = queue
            queue = {}
            -- entries may carry different origins; run them one by one, honouring nested @wait
            for i, e in ipairs(pending) do
                local n = e.cmd:match("^@wait%s+(%d+)$")
                if n then
                    local rest = {}
                    for j = i + 1, #pending do
                        rest[#rest + 1] = pending[j]
                    end
                    queue = rest
                    waitUpdates = math.max(tonumber(n), 1)
                    log(string.format("wait %d update(s) [%s]: %d command(s) deferred", waitUpdates, e.why, #rest))
                    break
                end
                exec(e.cmd, e.why)
            end
        end
    end
    if censusEvery > 0 then
        censusCount = censusCount + 1
        if censusCount >= censusEvery then
            censusCount = 0
            log(string.format("CENSUS %s time=%s %s", whereAmI(), gameTime(), census()))
        end
    end
end

-- One step of "@sweep <cmd> <first> <last> [<next>]": persist first, run second.  The file is
-- advanced (or removed after the last id) BEFORE the command runs, so if the command takes the
-- game down (that is what the sweep is for) the next launch plays the next id instead of
-- looping on the one that crashed; the log line "sweep <cmd> <n> of <first>..<last>" right
-- before the crash names the culprit.
local function sweepStep(parsed)
    local sw = parsed.sweep
    local n = sw.next
    if n > sw.last then
        Isaac.RemoveModData(mod)
        log(string.format("sweep %s: %d..%d already finished (next=%d) - save data removed, nothing run",
            sw.cmd, sw.first, sw.last, n))
        return
    end
    log(string.format("sweep %s %d of %d..%d", sw.cmd, n, sw.first, sw.last))
    if n >= sw.last then
        Isaac.RemoveModData(mod)
        log(string.format("sweep %s: %d is the last id - save data removed, HasModData=%s",
            sw.cmd, n, tostring(Isaac.HasModData(mod))))
    elseif type(Isaac.SaveModData) ~= "function" then
        log("ERROR sweep: Isaac.SaveModData is not a function - cannot advance, the same id will replay")
    else
        parsed.lines[sw.line] = string.format("@sweep %s %d %d %d", sw.cmd, sw.first, sw.last, n + 1)
        local raw = table.concat(parsed.lines, "\n") .. "\n"
        local ok, err = pcall(Isaac.SaveModData, mod, raw)
        if ok then
            log(string.format("sweep %s: next id %d saved (%d bytes)", sw.cmd, n + 1, #raw))
        else
            log(string.format("ERROR sweep save: %s", tostring(err)))
        end
    end
    exec(sw.cmd .. " " .. n, "sweep")
end

local function onGameStarted(_, continued)
    spec = nil
    bossFloorsDone = {}
    queue = {}
    waitUpdates = 0
    gameStarts = gameStarts + 1
    censusEvery = 0
    censusCount = 0
    local okSeed, seedNow = pcall(function() return Game():GetSeeds():GetStartSeedString() end)
    log(string.format("game start %d: seed=%s continued=%s", gameStarts,
        okSeed and tostring(seedNow) or "n/a", tostring(continued)))
    if not Isaac.HasModData(mod) then
        log("no save data (data/repro_console/save<slot>.dat missing) - nothing to do")
        return
    end
    local raw = Isaac.LoadModData(mod) or ""
    local parsed = parse(raw)
    spec = parsed
    local summary = string.format("save data: %d bytes, %d command(s), once=%s, continued=%s",
        #raw, parsed.n, tostring(parsed.once), tostring(continued))
    if parsed.sweep then
        summary = summary .. string.format(", sweep=%s %d..%d next=%d",
            parsed.sweep.cmd, parsed.sweep.first, parsed.sweep.last, parsed.sweep.next)
    end
    log(summary)
    log("state: " .. whereAmI())
    if continued and not parsed.continued then
        log("continued run: start commands skipped (add a line '@continue' to run them on Continue too)")
        return
    end
    runList(parsed.start, "start")
    if parsed.sweep then
        sweepStep(parsed)
        if parsed.once then
            log("once: ignored while a sweep is active (the sweep removes the file after its last id)")
        end
    elseif parsed.once then
        Isaac.RemoveModData(mod)
        log("once: save data removed, HasModData=" .. tostring(Isaac.HasModData(mod)))
    end
end

local function onNewLevel()
    if not spec then
        return
    end
    log("new level: " .. whereAmI())
    runList(spec.newlevel, "newlevel")
end

local function onNewRoom()
    if not spec then
        return
    end
    local game = Game()
    local room = game:GetRoom()
    if room:GetType() ~= RoomType.ROOM_BOSS then
        return
    end
    local level = game:GetLevel()
    local key = level:GetStage() .. ":" .. level:GetStageType()
    log(string.format("boss room: %s firstvisit=%s", whereAmI(), tostring(room:IsFirstVisit())))
    if bossFloorsDone[key] or #spec.bossroom == 0 then
        return
    end
    bossFloorsDone[key] = true
    runList(spec.bossroom, "bossroom")
end

mod:AddCallback(ModCallbacks.MC_POST_GAME_STARTED, onGameStarted)
mod:AddCallback(ModCallbacks.MC_POST_NEW_LEVEL, onNewLevel)
mod:AddCallback(ModCallbacks.MC_POST_NEW_ROOM, onNewRoom)
mod:AddCallback(ModCallbacks.MC_POST_UPDATE, onUpdate)

log(string.format("loaded (%s, Isaac.ExecuteCommand=%s); waiting for MC_POST_GAME_STARTED",
    tostring(_VERSION), type(Isaac.ExecuteCommand)))
