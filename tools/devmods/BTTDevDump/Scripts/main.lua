-- DEVELOPER TOOL (never shipped): dumps game internals needed for the player model, damage and
-- gore sync work. Waits until a gameplay map with NPCs is loaded, then:
--   1. GenerateSDK()  -> ue4ss/CXXHeaderDump (all loaded classes incl. Blueprints)
--   2. writes BTTDevDump.txt: NPC/player components, skeletal meshes, bone names, damage-related functions
local UEHelpers = require("UEHelpers")
local modDir = (debug.getinfo(1, "S").source:gsub("^@", "")):match("^(.*)[/\\][Ss]cripts[/\\]")
local out = io.open(modDir .. "\\BTTDevDump.txt", "w")
local function w(s) out:write(s .. "\n") out:flush() end
local function valid(o) return o and o.IsValid and o:IsValid() end
local done = false

local function classChain(obj)
    local names = {}
    local c = obj:GetClass()
    while valid(c) do
        names[#names + 1] = c:GetFullName()
        c = c:GetSuperStruct()
    end
    return table.concat(names, "  <  ")
end

local function dumpFunctions(cls, pattern)
    local c = cls
    while valid(c) do
        c:ForEachFunction(function(f)
            local n = f:GetFName():ToString()
            if not pattern or n:lower():find(pattern) then
                local params = {}
                f:ForEachProperty(function(p)
                    params[#params + 1] = p:GetClass():GetFName():ToString() .. " " .. p:GetFName():ToString()
                end)
                w("    fn " .. c:GetFName():ToString() .. "::" .. n .. "(" .. table.concat(params, ", ") .. ")")
            end
        end)
        c = c:GetSuperStruct()
    end
end

local function dumpProps(cls, pattern)
    local c = cls
    while valid(c) do
        c:ForEachProperty(function(p)
            local n = p:GetFName():ToString()
            if not pattern or n:lower():find(pattern) then
                w("    prop " .. c:GetFName():ToString() .. "." .. n .. " : " .. p:GetClass():GetFName():ToString())
            end
        end)
        c = c:GetSuperStruct()
    end
end

local function dumpActor(a, label)
    w("== " .. label .. ": " .. a:GetFullName())
    w("  class chain: " .. classChain(a))
    -- components referenced by the actor's own object properties (+ ACharacter::Mesh)
    local comps, seenc = {}, {}
    pcall(function() if valid(a.Mesh) then comps[#comps + 1] = a.Mesh end end)
    local c0 = a:GetClass()
    while valid(c0) do
        c0:ForEachProperty(function(p)
            if p:GetClass():GetFName():ToString() == "ObjectProperty" then
                local ok, v = pcall(function() return a[p:GetFName():ToString()] end)
                if ok and valid(v) and v:IsA(StaticFindObject("/Script/Engine.SceneComponent")) then comps[#comps + 1] = v end
            end
        end)
        c0 = c0:GetSuperStruct()
    end
    for _, c in ipairs(comps) do
        if valid(c) and not seenc[c:GetFullName()] then
            seenc[c:GetFullName()] = true
            w("  comp " .. c:GetFullName())
            if c:IsA(StaticFindObject("/Script/Engine.SkinnedMeshComponent")) then
                pcall(function() w("    mesh asset: " .. c.SkeletalMesh:GetFullName()) end)
                pcall(function()
                    local anim = c:GetAnimInstance()
                    if valid(anim) then w("    anim instance: " .. anim:GetFullName()) end
                end)
                pcall(function()
                    local n = c:GetNumBones()
                    local bones = {}
                    for i = 0, n - 1 do bones[#bones + 1] = c:GetBoneName(i):ToString() end
                    w("    bones (" .. n .. "): " .. table.concat(bones, " "))
                end)
            end
        end
    end
    w("  -- functions matching damage/hit/gore/death/ragdoll/health/kill/shot")
    for _, pat in ipairs({ "damage", "hit", "gore", "death", "dead", "die", "ragdoll", "health", "kill", "shot", "dismember", "bleed", "wound", "impact" }) do
        dumpFunctions(a:GetClass(), pat)
    end
    w("  -- properties matching health/dead/gore/damage/ragdoll/ai")
    for _, pat in ipairs({ "health", "dead", "gore", "damage", "ragdoll", "alive", "hp" }) do
        dumpProps(a:GetClass(), pat)
    end
end

local function run()
    w("map: " .. UEHelpers.GetWorld():GetFullName())
    local pawn = UEHelpers.GetPlayer()
    if valid(pawn) then dumpActor(pawn, "PLAYER PAWN") end
    local seen = {}
    for _, a in ipairs(FindAllOf("Character") or {}) do
        if valid(a) and a ~= pawn then
            local cn = a:GetClass():GetFName():ToString()
            if not seen[cn] then
                seen[cn] = true
                dumpActor(a, "CHARACTER")
            end
        end
    end
    -- damage-related interfaces / function libraries
    for _, path in ipairs({ "/Game/VRExpansion/Interfaces/DamageInterface.DamageInterface_C",
                            "/Game/WeaponMaster/Blueprints/Basic/BP_SimpleFirearm.BP_SimpleFirearm_C",
                            "/Game/WeaponMaster/Blueprints/Basic/BP_HeldProjectile.BP_HeldProjectile_C" }) do
        local c = StaticFindObject(path)
        if valid(c) then
            w("== CLASS " .. path)
            dumpFunctions(c, nil)
        else
            w("== CLASS " .. path .. " not loaded")
        end
    end
    w("== projectile / bullet classes loaded:")
    for _, a in ipairs(FindAllOf("Actor") or {}) do
        local cn = a:GetClass():GetFName():ToString()
        if (cn:find("Bullet") or cn:find("Projectile") or cn:find("Gore") or cn:find("Decal")) and not seen[cn] then
            seen[cn] = true
            w("  actor class " .. a:GetClass():GetFullName())
        end
    end
    w("-- generating SDK headers (CXXHeaderDump)...")
    local ok, err = pcall(GenerateSDK)
    w("GenerateSDK: " .. tostring(ok) .. " " .. tostring(err))
    w("DONE")
    out:close()
end

LoopInGameThreadAfterFrames(60, function()
    if done then return true end
    local world = UEHelpers.GetWorld()
    if not valid(world) or not world:GetFullName():find("Map_Sandbox") then return false end
    local chars = FindAllOf("Character") or {}
    if #chars < 2 then return false end  -- wait for NPCs to spawn
    done = true
    local ok, err = pcall(run)
    if not ok then pcall(function() w("ERROR " .. tostring(err)) out:close() end) end
    return true
end)
print("[BTTDevDump] waiting for Map_Sandbox with NPCs\n")
