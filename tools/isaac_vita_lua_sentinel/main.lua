local Sentinel = RegisterMod("repentogxm Lua Sentinel", 1)

print("REPENTOGXM LUA SENTINEL TOPLEVEL")

local gameStartedSeen = false
local updateSeen = false
local renderSeen = false

local function onGameStarted(_, continued)
    if gameStartedSeen then
        return
    end
    gameStartedSeen = true
    print("REPENTOGXM LUA SENTINEL GAME STARTED continued=" .. tostring(continued))
end

local function onUpdate()
    if updateSeen then
        return
    end
    updateSeen = true
    print("REPENTOGXM LUA SENTINEL POST UPDATE")
end

local function onRender()
    if renderSeen then
        return
    end
    renderSeen = true
    print("REPENTOGXM LUA SENTINEL POST RENDER")
end

Sentinel:AddCallback(ModCallbacks.MC_POST_GAME_STARTED, onGameStarted)
Sentinel:AddCallback(ModCallbacks.MC_POST_UPDATE, onUpdate)
Sentinel:AddCallback(ModCallbacks.MC_POST_RENDER, onRender)
