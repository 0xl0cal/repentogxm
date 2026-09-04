# EID pre-frame oracle

`test_vita_eid_startup.py` freezes the exact Repentance PE, EID 5.23
`bc0551a`, the shipping `enums.lua` / `main.lua` / `json.lua`, and pristine
Lua 5.3.3 sources. It then:

1. authenticates the PE `RegisterClasses` body and the frozen 1,105-row / 930
   target `lua_pushcclosure` census;
2. builds a fresh 32-bit Lua executable;
3. executes the real core bootstrap followed by the complete EID top level;
4. proves 102 module paths (including `json` from the core directory) and all
   35 callback additions in the core callback registry; and
5. repeats the run and requires a byte-identical transcript.

Native game objects are inert recording proxies. This deliberately proves the
pre-frame script/module/registration boundary only. It neither calls an EID
callback nor claims that rendering or gameplay-time Isaac API semantics work.
The next unproved boundary is the native engine's `_RunCallback` dispatch into
the first registered EID Lua function on Vita.

Example from the repository root:

```powershell
python recomp/test_vita_eid_startup.py `
  --pe D:\path\to\isaac-ng.exe.unpacked.exe `
  --eid 'E:\SteamLibrary\steamapps\common\The Binding of Isaac Rebirth\mods\external item descriptions_836319872' `
  --core-scripts D:\path\to\resources\scripts `
  --lua-source D:\path\to\lua-5.3.3
```
