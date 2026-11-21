-- DEVELOPER TOOL (never shipped): floor/pawn measurements + NPC animation variables.
local UEHelpers = require("UEHelpers")
local modDir = (debug.getinfo(1, "S").source:gsub("^@", "")):match("^(.*)[/\\][Ss]cripts[/\\]")
local out = io.open(modDir .. "\\BTTDevDump2.txt", "w")
local function w(s) out:write(s .. "\n") out:flush() end
local function valid(o) return (type(o) == "userdata" or type(o) == "table") and pcall(function() return o:IsValid() end) and o:IsValid() end
local done = {}

local function props(obj, label, pattern)
    local c = obj:GetClass()
    while valid(c) do
        c:ForEachProperty(function(p)
            local n = p:GetFName():ToString()
            if not pattern or n:lower():find(pattern) then
                local v = ""
                pcall(function()
                    local val = obj[n]
                    if type(val) == "number" or type(val) == "boolean" then v = tostring(val)
                    elseif type(val) == "userdata" and val.X then v = string.format("(%.1f,%.1f,%.1f)", val.X, val.Y, val.Z)
                    elseif type(val) == "userdata" and val.Pitch then v = string.format("(P%.1f,Y%.1f,R%.1f)", val.Pitch, val.Yaw, val.Roll) end
                end)
                w("  " .. label .. " " .. c:GetFName():ToString() .. "." .. n .. " : " .. p:GetClass():GetFName():ToString() .. " = " .. v)
            end
        end)
        c = c:GetSuperStruct()
    end
end

LoopInGameThreadAfterFrames(90, function()
    local world = UEHelpers.GetWorld()
    if not valid(world) then return false end
    local map = world:GetFullName()
    local pawn = UEHelpers.GetPlayer()
    if valid(pawn) and not done[map] then
        done[map] = true
        w("== MAP " .. map)
        w("pawn class: " .. pawn:GetClass():GetFullName())
        local l = pawn:K2_GetActorLocation()
        w(string.format("pawn actor Z=%.1f", l.Z))
        pcall(function() w(string.format("capsule half=%.1f, capsule comp Z=%.1f", pawn.CapsuleComponent:GetScaledCapsuleHalfHeight(), pawn.CapsuleComponent:K2_GetComponentLocation().Z)) end)
        pcall(function() w(string.format("root comp Z=%.1f (%s)", pawn:K2_GetRootComponent():K2_GetComponentLocation().Z, pawn:K2_GetRootComponent():GetFullName())) end)
        local cam = UEHelpers.GetPlayerController().PlayerCameraManager:GetCameraLocation()
        w(string.format("camera Z=%.1f", cam.Z))
        -- floor under the camera via line trace
        pcall(function()
            local ksl = UEHelpers.GetKismetSystemLibrary()
            local hit = {}
            local ok = ksl:LineTraceSingle(world, cam, { X = cam.X, Y = cam.Y, Z = cam.Z - 500 }, 0, false, {}, 0, hit, true, { R = 0, G = 0, B = 0, A = 0 }, { R = 0, G = 0, B = 0, A = 0 }, 0)
            w("trace returned " .. tostring(ok) .. " hit.Location=" .. tostring(hit.Location and hit.Location.Z) .. " ImpactPoint=" .. tostring(hit.ImpactPoint and hit.ImpactPoint.Z))
        end)
        props(pawn, "PAWN", "grip|held|weapon|equip|hand")
    end
    if not done.npc then
        for _, a in ipairs(FindAllOf("BP_NPC_Man_C") or {}) do
            if valid(a) then
                done.npc = true
                w("== NPC " .. a:GetFullName())
                local anim = a.Mesh:GetAnimInstance()
                w("anim: " .. anim:GetFullName())
                props(anim, "ANIM", nil)
                props(a, "NPC", "weapon|gun|held|armed|aim|look|head|ik|hand|pistol|equip|holster|unarmed|hostage|surrender|state")
                for _, n in ipairs({ "HeldObjectRoot", "Slidemesh", "StaticMesh", "SkeletalMesh", "BodyMesh" }) do
                    pcall(function()
                        local c = a[n]
                        local m = ""
                        pcall(function() m = c.StaticMesh:GetFullName() end)
                        pcall(function() m = m .. " " .. c.SkeletalMesh:GetFullName() end)
                        w("  comp " .. n .. " visible=" .. tostring(c:IsVisible()) .. " " .. m)
                    end)
                end
                break
            end
        end
    end
    if done.npc and done["World /Game/Maps/Sandbox/Map_Sandbox.Map_Sandbox"] then
        w("DONE")
        out:close()
        return true
    end
    return false
end)
