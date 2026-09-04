local ok, message = pcall(guest_fail)
assert(not ok)
assert(message == "guest boom")

local bad_ok = pcall(guest_badcheck)
assert(not bad_ok)

-- x86 cdecl varargs through luaL_error: every 5.3.3 conversion, rendered
-- identically to the native lua_pushfstring reference, longer than 255 bytes.
local format_ok, formatted = pcall(guest_format_fail)
assert(not format_ok)
assert(formatted == expected_format)
assert(#formatted > 255)

-- A Lua caller gets luaL_where(L, 1) in front, exactly like vanilla.
local where_ok, where_message = pcall(function() guest_format_fail() end)
assert(not where_ok)
local where_end = string.find(where_message, expected_format, 1, true)
assert(where_end ~= nil and where_end > 1)
assert(string.find(string.sub(where_message, 1, where_end - 1),
                   "main%.lua:%d+: $") ~= nil)

-- The frozen require's error, parsed with EID main.lua's own code.
local require_ok, err = pcall(guest_require_fail)
assert(not require_ok)
assert(err == "module '' not found:\n\tno file 'resources/scripts/.lua'"
    .. "\n\tno file 'ux0:data/isaacr001/mods/836319872/.lua'")
local _, basePathStart = string.find(err, "no file '", 1)
local _, modPathStart = string.find(err, "no file '", basePathStart)
local modPathEnd, _ = string.find(err, ".lua'", modPathStart)
local modPath = string.sub(err, modPathStart+1, modPathEnd-1)
modPath = string.gsub(modPath, "\\", "/")
modPath = string.gsub(modPath, "//", "/")
modPath = string.gsub(modPath, ":/", ":\\")
assert(modPath == "ux0:data/isaacr001/mods/836319872/")

assert(guest_pushfstring() == expected_format)

-- Malformed conversions raise vanilla's diagnostic instead of faulting.
local badformat_ok, badformat_message = pcall(guest_badformat)
assert(not badformat_ok)
assert(badformat_message == "invalid option '%q' to 'lua_pushfstring'")
local badpush_ok, badpush_message = pcall(guest_badpushfstring)
assert(not badpush_ok)
assert(badpush_message == "invalid option '%q' to 'lua_pushfstring'")

local nested = require("oracle_module")
assert(nested.value == 1)

local core = require("json")
assert(core.vita_core == true)

result = guest_add(nested.value)

-- LuaBridge-shaped __index fixture (wf/opt-lua-index).  Two identical class
-- hierarchies differ only in which closure is their __index: native_index
-- carries the frozen CFunc::indexMetaMethod address (replayed natively when
-- the bridge is built with ISAAC_VITA_LUA_NATIVE_INDEX), ref_index the same
-- oracle body under an address the bridge never intercepts.  Every scenario's
-- observable outcome (result count, types, values, error text) must agree
-- between the two closures here and, through the printed digest, between the
-- OFF and ON builds of the bridge.
local labels = {
  [string.len] = "string.len", [rawequal] = "rawequal",
  [guest_getter] = "guest_getter", [guest_getter_base] = "guest_getter_base",
  [guest_bad_getter] = "guest_bad_getter",
  [guest_boxed_getter] = "guest_boxed_getter", [guest_fail] = "guest_fail",
}

local function label(value)
  local kind = type(value)
  if value == nil then return "nil" end
  if kind == "number" or kind == "boolean" then
    return kind .. ":" .. tostring(value)
  end
  if kind == "string" then return "string:" .. value end
  if labels[value] then return kind .. ":" .. labels[value] end
  if kind == "userdata" then
    return "userdata:" .. tostring(labels[getmetatable(value)]) .. ":"
        .. tostring(guest_getter(value))
  end
  return kind .. ":?"
end

local function run(index_fn)
  local digest = {}
  local hits, fallbacks, nested_total = 0, 0, 0
  local IDENTITY = {}
  local base_mt = {
    __index = index_fn, __type = "Base", [IDENTITY] = true,
    __propget = { base_value = guest_getter_base },
    base_method = string.len,
  }
  base_mt.__const = { __index = index_fn, __type = "const Base",
                      __propget = {} }
  local derived_mt = {
    __index = index_fn, __type = "Derived", [IDENTITY] = true,
    __parent = base_mt,
    __propget = { value = guest_getter, boxed = guest_boxed_getter,
                  bad = guest_bad_getter, bad_prop = 5 },
    method = rawequal, fail = guest_fail, bad_value = 5,
  }
  derived_mt.__const = { __index = index_fn, __type = "const Derived",
                         __propget = {}, __parent = base_mt.__const }
  local no_propget_mt = { __index = index_fn, __type = "NoPropget" }
  local bad_propget_mt = { __index = index_fn, __type = "BadPropget",
                           __propget = 5 }
  local bad_parent_mt = { __index = index_fn, __type = "BadParent",
                          __propget = {}, __parent = 5 }
  local namespace_mt = { __index = index_fn,
                         __propget = { Version = guest_getter_base },
                         GetPlayer = string.len }
  labels[base_mt] = "Base"
  labels[derived_mt] = "Derived"
  labels[index_fn] = "index_fn"
  -- Hostile members (review): a Lua function reachable only after a
  -- __parent hop, builtin getters (assert passes the object through,
  -- collectgarbage raises luaL_argerror), a getter that re-enters __index
  -- through the bridge, and table.pack boxing a fresh table per call.
  base_mt.lua_method = function() end
  derived_mt.__propget.self = assert
  derived_mt.__propget.opt = collectgarbage
  derived_mt.__propget.nested = guest_nested_getter
  derived_mt.__propget.pack = table.pack
  local obj = guest_newobject(derived_mt, 0x1234)
  local base_obj = guest_newobject(base_mt, 0x77)
  local no_propget = guest_newobject(no_propget_mt, 1)
  local bad_propget = guest_newobject(bad_propget_mt, 2)
  local bad_parent = guest_newobject(bad_parent_mt, 3)
  local namespace = setmetatable({}, namespace_mt)

  -- outcome: "hit" (the __index returns natively), "fallback" (the native
  -- replay hands the call to the translated body), "raise" (the getter
  -- itself raises before any result exists).
  -- nested: extra native __index hits a getter causes by re-entering the
  -- bridge (counted once here; a VERIFY re-run of the outer body repeats them).
  local function scenario(name, outcome, fn, nested)
    local results = table.pack(pcall(fn))
    local parts = { name }
    if results[1] then
      parts[#parts + 1] = "n=" .. (results.n - 1)
      for i = 2, results.n do parts[#parts + 1] = label(results[i]) end
    else
      parts[#parts + 1] = "err=" .. tostring(results[2])
    end
    digest[#digest + 1] = table.concat(parts, " ")
    if outcome == "hit" then hits = hits + 1 + (nested or 0) end
    if outcome == "fallback" then fallbacks = fallbacks + 1 end
    nested_total = nested_total + (nested or 0)
  end

  scenario("method", "hit", function() return obj.method end)
  scenario("method-call", "hit", function() return obj.method(obj, obj) end)
  scenario("parent-method", "hit", function() return obj.base_method end)
  scenario("parent-method-call", "hit",
           function() return obj.base_method("abcd") end)
  scenario("property", "hit", function() return obj.value end)
  scenario("parent-property", "hit", function() return obj.base_value end)
  scenario("boxed-property", "hit", function() return obj.boxed end)
  scenario("base-property", "hit", function() return base_obj.base_value end)
  scenario("getter-error", "raise", function() return obj.bad end)
  scenario("method-error", "hit", function() return obj.fail() end)
  scenario("nil", "hit", function() return obj.nothing end)
  scenario("nil-integer-key", "hit", function() return obj[1] end)
  scenario("namespace-method", "hit",
           function() return namespace.GetPlayer end)
  scenario("namespace-property", "hit",
           function() return namespace.Version end)
  scenario("namespace-nil", "hit", function() return namespace.Missing end)
  scenario("not-cfunction", "fallback", function() return obj.bad_value end)
  scenario("const-table", "fallback", function() return obj.__const end)
  scenario("propget-not-cfunction", "fallback",
           function() return obj.bad_prop end)
  scenario("missing-propget", "fallback", function() return no_propget.x end)
  scenario("propget-not-table", "fallback",
           function() return bad_propget.x end)
  scenario("parent-not-table", "fallback", function() return bad_parent.x end)
  scenario("no-metatable", "fallback", function() return index_fn({}, {}) end)
  -- Hostile cases (review): arity other than 2 (the body indexes the live
  -- stack, so with one argument "index 2" is the metatable itself), a
  -- fallback after a __parent hop and one with a single argument, builtin
  -- getters, re-entrant __index inside a getter, non-string keys, the
  -- __index key itself.
  scenario("arity-1", "hit", function() return index_fn(obj) end)
  scenario("arity-3", "hit",
           function() return index_fn(obj, "method", "extra") end)
  scenario("arity-1-fallback", "fallback",
           function() return index_fn(no_propget) end)
  scenario("hop-then-lua-function", "fallback",
           function() return obj.lua_method end)
  scenario("getter-self", "hit", function() return obj.self end)
  scenario("getter-argerror", "raise", function() return obj.opt end)
  scenario("getter-nested", "hit", function() return obj.nested end, 1)
  scenario("nil-bool-key", "hit", function() return obj[true] end)
  scenario("nil-nan-key", "hit", function() return obj[0/0] end)
  scenario("self-index-key", "hit", function() return obj.__index end)
  if index_trip == 1 then
    -- VERIFY trip (oracle argv[2] == "trip"): table.pack returns a fresh
    -- table per call, so the re-run's result slot compares unequal; the
    -- bridge must log the mismatch, disable the replay and keep serving
    -- identical results through the fallback (reason "off").
    scenario("trip-fresh-table", "hit", function() return obj.pack end)
    scenario("post-trip-method", "fallback", function() return obj.method end)
    scenario("post-trip-property", "fallback",
             function() return obj.value end)
    scenario("post-trip-nil", "fallback", function() return obj.nothing end)
  end
  return digest, hits, fallbacks, nested_total
end

local native_digest, native_hits, native_fallbacks, native_nested =
  run(native_index)
local ref_digest = run(ref_index)
local scenario_count = index_trip == 1 and 36 or 32
assert(#native_digest == scenario_count and #ref_digest == scenario_count)
for i = 1, #native_digest do
  assert(native_digest[i] == ref_digest[i],
         native_digest[i] .. " ~= " .. ref_digest[i])
end
assert(native_hits == (index_trip == 1 and 23 or 22), native_hits)
assert(native_fallbacks == (index_trip == 1 and 12 or 9), native_fallbacks)
assert(native_nested == 1)
assert(native_digest[1] == "method n=1 function:rawequal")
assert(native_digest[2] == "method-call n=1 boolean:true")
assert(native_digest[3] == "parent-method n=1 function:string.len")
assert(native_digest[4] == "parent-method-call n=1 number:4")
assert(native_digest[5] == "property n=1 number:4660")
assert(native_digest[6] == "parent-property n=1 number:77")
assert(native_digest[7] == "boxed-property n=1 userdata:Derived:4660")
assert(native_digest[8] == "base-property n=1 number:77")
-- luaL_where(L, 1) from the getter names its caller, the C __index closure:
-- no position, in both the native replay and the translated body.
assert(native_digest[9] == "getter-error err=bad getter Prop")
-- The method value is the same closure either way; called from Lua it keeps
-- the Lua caller's position prefix.
assert(string.find(native_digest[10],
                   "^method%-error err=.*main%.lua:%d+: guest boom$") ~= nil)
assert(native_digest[11] == "nil n=1 nil")
assert(native_digest[12] == "nil-integer-key n=1 nil")
assert(native_digest[13] == "namespace-method n=1 function:string.len")
assert(native_digest[14] == "namespace-property n=1 number:77")
assert(native_digest[15] == "namespace-nil n=1 nil")
-- The fallback re-runs the translated body from the entry stack: its throw
-- text, prefixed by the Lua caller's position, is what a mod would see.
for i = 16, 18 do
  assert(string.find(native_digest[i],
                     "main%.lua:%d+: logic_error: not a cfunction$") ~= nil,
         native_digest[i])
end
for i = 19, 20 do
  assert(string.find(native_digest[i],
         "main%.lua:%d+: logic_error: missing __propget table$") ~= nil,
         native_digest[i])
end
assert(string.find(native_digest[21],
       "main%.lua:%d+: logic_error: __parent is not a table$") ~= nil,
       native_digest[21])
-- Called directly (no metatable, unchecked lua_getmetatable) the key table
-- doubles as the "metatable": rawget finds no __propget in it, so the body
-- throws that text; the native replay fell back before its first push.
assert(string.find(native_digest[22],
       "^no%-metatable err=.*main%.lua:%d+: logic_error: missing __propget table$")
       ~= nil, native_digest[22])

-- Hostile cases.
assert(native_digest[23] == "arity-1 n=1 nil")
assert(native_digest[24] == "arity-3 n=1 function:rawequal")
assert(string.find(native_digest[25],
       "^arity%-1%-fallback err=.*main%.lua:%d+: logic_error: missing __propget table$")
       ~= nil, native_digest[25])
assert(string.find(native_digest[26],
       "^hop%-then%-lua%-function err=.*main%.lua:%d+: logic_error: not a cfunction$")
       ~= nil, native_digest[26])
assert(native_digest[27] == "getter-self n=1 userdata:Derived:4660")
-- luaL_argerror from a builtin called by the C __index closure: no position,
-- the global name found through package.loaded.
assert(string.find(native_digest[28],
       "^getter%-argerror err=bad argument #1 to '[%w?]+' %(string expected, got userdata%)$")
       ~= nil, native_digest[28])
assert(native_digest[29] == "getter-nested n=1 number:4660")
assert(native_digest[30] == "nil-bool-key n=1 nil")
assert(native_digest[31] == "nil-nan-key n=1 nil")
assert(native_digest[32] == "self-index-key n=1 function:index_fn")
if index_trip == 1 then
  assert(native_digest[33] == "trip-fresh-table n=1 table:?")
  assert(native_digest[34] == "post-trip-method n=1 function:rawequal")
  assert(native_digest[35] == "post-trip-property n=1 number:4660")
  assert(native_digest[36] == "post-trip-nil n=1 nil")
end

index_digest = table.concat(native_digest, "\n")
index_expected_hits = native_hits
index_expected_fallbacks = native_fallbacks
index_expected_nested = native_nested
