-- Execute the frozen Repentance core Lua bootstrap and EID's complete
-- top-level startup without advancing a game frame.  Unknown native Isaac
-- objects are inert recording proxies.  The core callback implementation is
-- real; registered callback functions are deliberately never invoked.

local eid_root = assert(arg[1], "missing EID root")
local core_script_root = assert(arg[2], "missing core script root")
local output_path = assert(arg[3], "missing transcript path")
local dispatch_callbacks = arg[4] == "dispatch"
local host_debug = debug
local host_loadfile = loadfile
local host_traceback = debug.traceback

local records = {}
local seen = {}

local function record(kind, value)
    local row = kind .. "\t" .. value
    if not seen[row] then
        seen[row] = true
        records[#records + 1] = row
    end
end

local numeric_fields = {
    X = true, Y = true, Z = true, R = true, G = true, B = true, A = true,
    RO = true, GO = true, BO = true, Red = true, Green = true, Blue = true,
    Alpha = true, Size = true, Frame = true, InitSeed = true, SubType = true,
    Variant = true, Type = true, ControllerIndex = true, Index = true,
    GridIndex = true, Charge = true, MaxCharge = true, Quality = true,
}

local proxy_mt = {}
local proxy_cache = {}

local function proxy(name)
    local value = proxy_cache[name]
    if value ~= nil then return value end
    value = setmetatable({ __oracle_name = name }, proxy_mt)
    proxy_cache[name] = value
    return value
end

local function call_result(name)
    if name:match("[.:]Load$") or name:match("[.:]IsLoaded$") then
        return true
    end
    if name:match("[.:]HasData$") or name:match("[.:]IsFinished$") then
        return false
    end
    if name:match("[.:]GetName$") or name:match("[.:]GetAnimation$") or
       name:match("[.:]GetFilename$") then
        return ""
    end
    if name:match("[.:]GetNum") or name:match("[.:]GetFrame") or
       name:match("[.:]GetCount") or name:match("[.:]GetValue") or
       name:match("[.:]GetID") or name:match("[.:]GetType") or
       name:match("[.:]GetVariant") or name:match("[.:]GetSubType") or
       name:match("[.:]GetGridIndex") then
        return 0
    end
    if name:match("[.:]Exists$") or name:match("[.:]Has") or
       name:match("[.:]Is") or name:match("[.:]Can") then
        return false
    end
    return proxy(name .. "()")
end

function proxy_mt.__index(value, key)
    if key == "__oracle_name" then return rawget(value, key) end
    local name = rawget(value, "__oracle_name") .. "." .. tostring(key)
    record("field", name)
    if numeric_fields[key] then return 0 end
    return proxy(name)
end

function proxy_mt.__newindex(value, key, item)
    record("write", rawget(value, "__oracle_name") .. "." .. tostring(key))
    rawset(value, key, item)
end

function proxy_mt.__call(value, ...)
    local name = rawget(value, "__oracle_name")
    record("call", name)
    return call_result(name)
end

function proxy_mt.__len() return 0 end
function proxy_mt.__pairs() return next, {}, nil end
function proxy_mt.__ipairs() return ipairs({}) end
function proxy_mt.__tostring(value) return rawget(value, "__oracle_name") end
function proxy_mt.__concat(a, b) return tostring(a) .. tostring(b) end
function proxy_mt.__add() return 0 end
function proxy_mt.__sub() return 0 end
function proxy_mt.__mul() return 0 end
function proxy_mt.__div() return 0 end
function proxy_mt.__mod() return 0 end
function proxy_mt.__pow() return 0 end
function proxy_mt.__unm() return 0 end
function proxy_mt.__band() return 0 end
function proxy_mt.__bor() return 0 end
function proxy_mt.__bxor() return 0 end
function proxy_mt.__shl() return 0 end
function proxy_mt.__shr() return 0 end
function proxy_mt.__lt() return false end
function proxy_mt.__le() return false end

local function make_native_class(name)
    local class_mt = {}
    class_mt.__index = function(value, key)
        local member = rawget(class_mt, key)
        if member ~= nil then return member end
        local full_name = rawget(value, "__oracle_name") .. "." .. tostring(key)
        record("field", full_name)
        if numeric_fields[key] then return 0 end
        return proxy(full_name)
    end
    class_mt.__newindex = proxy_mt.__newindex
    class_mt.__len = proxy_mt.__len
    class_mt.__tostring = proxy_mt.__tostring
    setmetatable(class_mt, {
        -- The shipping bootstrap saves native methods from the metatable
        -- before replacing them with default-argument wrappers.
        __index = function(table_value, key)
            local full_name = name .. ".<native>." .. tostring(key)
            record("native_method", full_name)
            local value = proxy(full_name)
            rawset(table_value, key, value)
            return value
        end,
    })

    local function constructor(...)
        record("call", name)
        return setmetatable({ __oracle_name = name .. "()" }, class_mt)
    end
    return constructor, class_mt
end

-- These are the exact classes which the frozen resources/scripts/main.lua
-- passes to BeginClass().  Each constructor returns a distinct native-class
-- metatable so that the bootstrap's compatibility wrappers are exercised.
for _, name in ipairs({
    "Font", "ItemPool", "SFXManager", "HUD", "TemporaryEffects", "Room",
    "MusicManager", "Game", "Level", "Sprite", "EntityTear", "EntityBomb",
    "EntityKnife", "EntityLaser", "EntityProjectile", "EntityFamiliar",
    "EntityNPC", "EntityPickup", "EntityPlayer",
}) do
    rawset(_G, name, (make_native_class(name)))
end
-- The core bootstrap installs the public Game constructor from this native
-- overload after it has attached compatibility methods to Game's userdata.
Game_0 = Game

local function make_value_class(name, constructor)
    local class_mt = {}
    class_mt.__index = class_mt
    local class = setmetatable({}, {
        __class = class_mt,
        __call = function(_, ...)
            record("call", name)
            return setmetatable(constructor(...), class_mt)
        end,
    })
    return class, class_mt
end

local vector_mt
Vector, vector_mt = make_value_class("Vector", function(x, y)
    return { __oracle_name = "Vector()", X = x or 0, Y = y or 0 }
end)
function vector_mt.__add(a, b) return Vector((a.X or 0)+(b.X or 0), (a.Y or 0)+(b.Y or 0)) end
function vector_mt.__sub(a, b) return Vector((a.X or 0)-(b.X or 0), (a.Y or 0)-(b.Y or 0)) end
function vector_mt.__mul(a, b)
    if type(a) == "number" then a, b = b, a end
    if type(b) == "table" then return Vector((a.X or 0)*(b.X or 0), (a.Y or 0)*(b.Y or 0)) end
    return Vector((a.X or 0)*b, (a.Y or 0)*b)
end
function vector_mt.__div(a, b) return Vector((a.X or 0)/b, (a.Y or 0)/b) end
function vector_mt.__unm(a) return Vector(-(a.X or 0), -(a.Y or 0)) end
function vector_mt:Length() return 0 end
function vector_mt:Distance() return 0 end
function vector_mt:Normalized() return self end
function vector_mt:Resized() return self end
function vector_mt:Rotated() return self end

local color_mt
Color, color_mt = make_value_class("Color", function(red, green, blue, alpha,
                                                       red_offset, green_offset,
                                                       blue_offset)
    return {
        __oracle_name = "Color()",
        R = red or 0, G = green or 0, B = blue or 0, A = alpha or 0,
        RO = red_offset or 0, GO = green_offset or 0, BO = blue_offset or 0,
        Red = red or 0, Green = green or 0, Blue = blue or 0,
        Alpha = alpha or 0,
    }
end)

KColor = make_value_class("KColor", function(red, green, blue, alpha)
    return {
        __oracle_name = "KColor()",
        Red = red or 0, Green = green or 0, Blue = blue or 0,
        Alpha = alpha or 0,
    }
end)

BitSet128 = make_value_class("BitSet128", function(low, high)
    return { __oracle_name = "BitSet128()", l = low or 0, h = high or 0 }
end)

Isaac = setmetatable({}, {
    __index = function(table_value, key)
        local name = "Isaac." .. tostring(key)
        record("field", name)
        local value = proxy(name)
        rawset(table_value, key, value)
        return value
    end,
})

-- Native leaves used by the core callback layer need their real return
-- semantics; all other native APIs stay inert and are recorded by the proxy.
function Isaac.RegisterMod(mod, name, api_version)
    record("call", "Isaac.RegisterMod")
    rawset(mod, "Name", name)
    rawset(mod, "ApiVersion", api_version)
end
function Isaac.SetBuiltInCallbackState(callback_id, enabled)
    record("callback_state", tostring(callback_id) .. "=" .. tostring(enabled))
end
function Isaac.HasModData() record("call", "Isaac.HasModData"); return false end
function Isaac.LoadModData() record("call", "Isaac.LoadModData"); return "" end
function Isaac.SaveModData() record("call", "Isaac.SaveModData") end
function Isaac.RemoveModData() record("call", "Isaac.RemoveModData") end

Options = { Language = 0, HUDOffset = 0 }
Input = proxy("Input")
ItemConfig = proxy("ItemConfig")
RoomDescriptor = proxy("RoomDescriptor")
_LUADEBUG = false
REPENTANCE_PLUS = false
REPENTOGON = nil
FontRenderSettings = nil

local absent_globals = {
    REPENTOGON = true, REPENTANCE_PLUS = true, FontRenderSettings = true,
    ModConfigMenu = true, EID = true, Controller = true,
    debug = true, arg = true, dofile = true, loadfile = true,
    __eidCardDescriptions = true, __eidEntityDescriptions = true,
    __eidItemDescriptions = true, __eidItemTransformations = true,
    __eidPillDescriptions = true, __eidTrinketDescriptions = true,
}
setmetatable(_G, {
    __index = function(table_value, key)
        if absent_globals[key] then return nil end
        local name = tostring(key)
        record("global", name)
        local value = proxy(name)
        rawset(table_value, key, value)
        return value
    end,
})

local separator = package.config:sub(1, 1)
local eid_pattern = eid_root:gsub("\\", "/")
local core_pattern = core_script_root:gsub("\\", "/")
package.path = eid_pattern .. "/?.lua;" .. core_pattern .. "/?.lua;" ..
               package.path

local function stable_path(path)
    path = path:gsub("\\", "/")
    if path:sub(1, #eid_pattern) == eid_pattern then
        return "<eid>" .. path:sub(#eid_pattern + 1)
    end
    if path:sub(1, #core_pattern) == core_pattern then
        return "<core>" .. path:sub(#core_pattern + 1)
    end
    return path
end

local original_require = require
require = function(name)
    record("require", tostring(name))
    local resolved = package.searchpath(name, package.path)
    if resolved then
        record("require_path", tostring(name) .. "\t" .. stable_path(resolved))
    end
    return original_require(name)
end

local function run_file(root, relative_path, label)
    local chunk, load_error = host_loadfile(root .. separator .. relative_path)
    assert(chunk, load_error)
    chunk()
    record("script", label)
end

local callback_names = {}
local callbacks = {}

local function callback_name(callback_id)
    return callback_names[callback_id] or tostring(callback_id)
end

local function stable_source(fn)
    local info = host_debug.getinfo(fn, "S")
    local source = (info and info.source or "?"):gsub("\\", "/")
    local root = eid_pattern
    if source:sub(1, 1) == "@" then source = source:sub(2) end
    if source:sub(1, #root) == root then
        source = "<eid>" .. source:sub(#root + 1)
    end
    return source .. ":" .. tostring(info and info.linedefined or -1)
end

local function stable_parameter(value)
    if value == nil then return "nil" end
    if type(value) == "number" or type(value) == "boolean" or
       type(value) == "string" then
        return tostring(value)
    end
    return "<" .. type(value) .. ">"
end

local startup_ok, startup_error = xpcall(function()
    run_file(core_script_root, "enums.lua", "<core>/enums.lua")
    for name, value in pairs(ModCallbacks) do
        if type(value) == "number" then
            callback_names[value] = "ModCallbacks." .. name
        end
    end

    -- This installs the shipping RegisterMod and callback registry, then all
    -- compatibility wrappers.  It is not a reimplementation in this harness.
    run_file(core_script_root, "main.lua", "<core>/main.lua")

    local core_add_priority = assert(Isaac.AddPriorityCallback)
    Isaac.AddPriorityCallback = function(mod, callback_id, priority, fn, param)
        core_add_priority(mod, callback_id, priority, fn, param)
        callbacks[#callbacks + 1] = {
            id = callback_id, priority = priority, fn = fn, param = param,
        }
        records[#records + 1] = string.format(
            "callback\t%03d\t%s\tpriority=%s\tparam=%s\t%s",
            #callbacks, callback_name(callback_id), tostring(priority),
            stable_parameter(param), stable_source(fn))
    end

    run_file(eid_root, "main.lua", "<eid>/main.lua")

    local ids = {}
    for _, callback in ipairs(callbacks) do ids[callback.id] = true end
    local registry_total = 0
    for callback_id in pairs(ids) do
        local registered = Isaac.GetCallbacks(callback_id, false)
        registry_total = registry_total + #registered
        record("callback_registry",
               callback_name(callback_id) .. "=" .. tostring(#registered))
    end
    assert(registry_total == #callbacks,
           "callback registry/capture count mismatch: " ..
           tostring(registry_total) .. " != " .. tostring(#callbacks))

    if dispatch_callbacks then
        for _, callback in ipairs(callbacks) do
            if callback.id == ModCallbacks.MC_POST_GAME_STARTED then
                Isaac.RunCallback(callback.id, false)
            else
                Isaac.RunCallback(callback.id)
            end
            record("callback_dispatch", callback_name(callback.id))
        end
    end
end, host_traceback)

record("result", startup_ok and "ok" or tostring(startup_error))
table.sort(records)

local output = assert(io.open(output_path, "wb"))
for _, row in ipairs(records) do output:write(row, "\n") end
output:write("callback_count\t", tostring(#callbacks), "\n")
output:close()

if not startup_ok then
    io.stderr:write(tostring(startup_error), "\n")
    os.exit(1)
end
