-- BloodTrail Together - UE4SS Lua side (game thread).
-- Responsibilities:
--   * load bin\btt_core.dll (networking, Unity Relay, UI window) with package.loadlib(path, "*")
--   * every frame: read the local VR pose (HMD + both motion controllers, world space) and the
--     current map, exchange one frame with the DLL over a named pipe, then draw the remote avatar,
--     play shared sounds, follow the host's map and show notifications.
-- Everything that touches the game is wrapped in pcall: a failure disables that feature and is
-- logged to UE4SS.log / BloodTrailTogether.log instead of crashing the game.

local VERSION = "0.1.0"
local TAG = "[BloodTrailTogether] "
local UEHelpers = require("UEHelpers")

local KEY_TOGGLE_WINDOW = Key.F9
local FRAME_MS = 11            -- ~90 Hz request rate
local MAP_POLL_FRAMES = 30     -- read the world name about 3x per second
local CONTROLLER_RESCAN_FRAMES = 120

local function log(msg) print(TAG .. tostring(msg) .. "\n") end

------------------------------------------------------------------------------- paths / DLL
local script = debug.getinfo(1, "S").source:gsub("^@", "")
local modDir = script:match("^(.*)[/\\][Ss]cripts[/\\]") or "."
local dllPath = modDir .. "\\bin\\btt_core.dll"

if type(package) ~= "table" or type(package.loadlib) ~= "function" then
    log("FATAL: this UE4SS build has no package.loadlib; BloodTrail Together cannot start")
    return
end
local loaded, loadErr = package.loadlib(dllPath, "*")
if not loaded then
    log("FATAL: could not load " .. dllPath .. ": " .. tostring(loadErr))
    return
end
log("loaded " .. dllPath)

------------------------------------------------------------------------------- pipe bridge
local pipeUp, pipeDown = nil, nil
local bridgeFailures = 0

local function openBridge()
    for n = 1, 8 do
        local base = "\\\\.\\pipe\\BloodTrailTogether_" .. n
        local up = io.open(base .. "_up", "wb")
        if up then
            local down = io.open(base .. "_down", "rb")
            if down then
                up:setvbuf("no")
                down:setvbuf("no")
                pipeUp, pipeDown = up, down
                log("bridge connected (pipe set " .. n .. ")")
                return true
            end
            up:close()
        end
    end
    return false
end

local function closeBridge()
    pcall(function() if pipeUp then pipeUp:close() end end)
    pcall(function() if pipeDown then pipeDown:close() end end)
    pipeUp, pipeDown = nil, nil
end

-- Sends one request frame and returns the reply text (or nil).
local function exchange(req)
    if not pipeUp then return nil end
    local ok, reply = pcall(function()
        pipeUp:write(string.format("%06d", #req), req)
        local hdr = pipeDown:read(6)
        if not hdr then return nil end
        local n = tonumber(hdr)
        if not n then return nil end
        if n == 0 then return "" end
        return pipeDown:read(n)
    end)
    if not ok or reply == nil then
        closeBridge()
        bridgeFailures = bridgeFailures + 1
        return nil
    end
    return reply
end

------------------------------------------------------------------------------- math
local DEG, RAD = math.pi / 180, 180 / math.pi

-- FRotator -> FQuat, same formula as UE4 FRotator::Quaternion
local function rotToQuat(p, y, r)
    local sp, cp = math.sin(p * DEG / 2), math.cos(p * DEG / 2)
    local sy, cy = math.sin(y * DEG / 2), math.cos(y * DEG / 2)
    local sr, cr = math.sin(r * DEG / 2), math.cos(r * DEG / 2)
    return cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy, cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy
end

local function normAxis(a)
    a = a % 360
    if a > 180 then a = a - 360 end
    return a
end

-- FQuat -> FRotator, same formula as UE4 FQuat::Rotator
local function quatToRot(x, y, z, w)
    local test = z * x - w * y
    local yawY, yawX = 2 * (w * z + x * y), 1 - 2 * (y * y + z * z)
    local yaw = math.atan(yawY, yawX) * RAD
    if test < -0.4999995 then
        return -90, yaw, normAxis(-yaw - 2 * math.atan(x, w) * RAD)
    elseif test > 0.4999995 then
        return 90, yaw, normAxis(yaw - 2 * math.atan(x, w) * RAD)
    end
    return math.asin(2 * test) * RAD, yaw, math.atan(-2 * (w * x + y * z), 1 - 2 * (x * x + y * y)) * RAD
end

-- Rotate vector (vx,vy,vz) by quaternion
local function qrot(x, y, z, w, vx, vy, vz)
    local tx = 2 * (y * vz - z * vy)
    local ty = 2 * (z * vx - x * vz)
    local tz = 2 * (x * vy - y * vx)
    return vx + w * tx + (y * tz - z * ty), vy + w * ty + (z * tx - x * tz), vz + w * tz + (x * ty - y * tx)
end

local function dist2(ax, ay, az, bx, by, bz)
    local dx, dy, dz = ax - bx, ay - by, az - bz
    return dx * dx + dy * dy + dz * dz
end

------------------------------------------------------------------------------- game access helpers
local function valid(o)
    local t = type(o)
    if t ~= "userdata" and t ~= "table" then return false end
    local ok, v = pcall(function() return o:IsValid() end)
    return ok and v == true
end

local function objPath(o)
    -- "SoundCue /Game/Audio/x/Cue.Cue" -> "/Game/Audio/x/Cue.Cue"
    local full = o:GetFullName()
    return full:match("^%S+%s+(.+)$") or full
end

local function currentMapPath()
    local world = UEHelpers.GetWorld()
    if not valid(world) then return nil end
    local p = objPath(world)                 -- /Game/Maps/Sandbox/Map_Sandbox.Map_Sandbox
    return p:match("^(.-)%.[^/]+$") or p     -- /Game/Maps/Sandbox/Map_Sandbox
end

local assetCache = {}
local function findAsset(path)
    local o = assetCache[path]
    if valid(o) then return o end
    o = StaticFindObject(path)
    if not valid(o) then
        pcall(LoadAsset, path:match("^(.-)%.") or path)
        o = StaticFindObject(path)
    end
    if valid(o) then assetCache[path] = o return o end
    return nil
end

------------------------------------------------------------------------------- local VR pose
local controllers = { left = nil, right = nil }
local frameNo = 0

local function scanControllers()
    controllers.left, controllers.right = nil, nil
    local all = FindAllOf("MotionControllerComponent")
    if not all then return end
    for _, c in ipairs(all) do
        if valid(c) then
            local okT, tracked = pcall(function() return c:IsTracked() end)
            local okS, src = pcall(function() return c.MotionSource:ToString() end)
            if okT and tracked and okS then
                if src == "Left" and not controllers.left then controllers.left = c end
                if src == "Right" and not controllers.right then controllers.right = c end
            end
        end
    end
end

local function compPose(c)
    local l = c:K2_GetComponentLocation()
    local r = c:K2_GetComponentRotation()
    local qx, qy, qz, qw = rotToQuat(r.Pitch, r.Yaw, r.Roll)
    return l.X, l.Y, l.Z, qx, qy, qz, qw
end

local localHead = { 0, 0, 0 }
local localLeft, localRight = { 0, 0, 0 }, { 0, 0, 0 }

-- Returns the "P ..." request line or nil
local function readLocalPose()
    local pc = UEHelpers.GetPlayerController()
    if not valid(pc) then return nil end
    local cam = pc.PlayerCameraManager
    if not valid(cam) then return nil end
    local hl = cam:GetCameraLocation()
    local hr = cam:GetCameraRotation()
    local hx, hy, hz = hl.X, hl.Y, hl.Z
    local qx, qy, qz, qw = rotToQuat(hr.Pitch, hr.Yaw, hr.Roll)
    localHead[1], localHead[2], localHead[3] = hx, hy, hz

    if frameNo % CONTROLLER_RESCAN_FRAMES == 1 or not valid(controllers.left) or not valid(controllers.right) then
        if frameNo % 30 == 1 then pcall(scanControllers) end
    end
    local flags = 0
    local L = { hx, hy, hz - 40, 0, 0, 0, 1 }
    local R = { hx, hy, hz - 40, 0, 0, 0, 1 }
    if valid(controllers.left) then
        local ok, a, b, c, d, e, f, g = pcall(compPose, controllers.left)
        if ok then L = { a, b, c, d, e, f, g } flags = flags | 1 end
    end
    if valid(controllers.right) then
        local ok, a, b, c, d, e, f, g = pcall(compPose, controllers.right)
        if ok then R = { a, b, c, d, e, f, g } flags = flags | 2 end
    end
    localLeft[1], localLeft[2], localLeft[3] = L[1], L[2], L[3]
    localRight[1], localRight[2], localRight[3] = R[1], R[2], R[3]
    -- Body: floor point under the head (pawn capsule bottom), facing the head's yaw.
    local floorZ = hz - 165
    pcall(function()
        local pawn = UEHelpers.GetPlayer()
        if valid(pawn) then
            local pl = pawn:K2_GetActorLocation()
            floorZ = pl.Z - pawn.CapsuleComponent:GetScaledCapsuleHalfHeight()
        end
    end)
    local yaw = select(2, quatToRot(qx, qy, qz, qw))
    local bqx, bqy, bqz, bqw = rotToQuat(0, yaw, 0)
    return string.format("P %.2f %.2f %.2f %.5f %.5f %.5f %.5f %.2f %.2f %.2f %.5f %.5f %.5f %.5f %.2f %.2f %.2f %.5f %.5f %.5f %.5f %.2f %.2f %.2f %.5f %.5f %.5f %.5f %d",
        hx, hy, hz, qx, qy, qz, qw, L[1], L[2], L[3], L[4], L[5], L[6], L[7], R[1], R[2], R[3], R[4], R[5], R[6], R[7],
        hx, hy, floorZ, bqx, bqy, bqz, bqw, flags)
end

------------------------------------------------------------------------------- remote avatar
local avatar = nil   -- { head, visor, left, right, tag }
local avatarDisabled = false
local IDENTITY = { Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Translation = { X = 0, Y = 0, Z = -100000 }, Scale3D = { X = 1, Y = 1, Z = 1 } }

local function spawnActor(classPath)
    local world = UEHelpers.GetWorld()
    local gs = UEHelpers.GetGameplayStatics()
    local cls = StaticFindObject(classPath)
    if not (valid(world) and valid(gs) and valid(cls)) then return nil end
    local a = gs:BeginDeferredActorSpawnFromClass(world, cls, IDENTITY, 1, nil)
    if not valid(a) then return nil end
    gs:FinishSpawningActor(a, IDENTITY)
    if not valid(a) then return nil end
    pcall(function() a:SetActorEnableCollision(false) end)
    return a
end

local function meshActor(meshPath, sx, sy, sz, r, g, b)
    local a = spawnActor("/Script/Engine.StaticMeshActor")
    if not a then return nil end
    local smc = a.StaticMeshComponent
    smc:SetMobility(2)                     -- Movable
    smc:SetStaticMesh(findAsset(meshPath))
    smc:SetCollisionEnabled(0)
    pcall(function() smc:SetCastShadow(false) end)
    a:SetActorScale3D({ X = sx, Y = sy, Z = sz })
    pcall(function()
        local mat = findAsset("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial")
        local mid = smc:CreateDynamicMaterialInstance(0, mat, FName("BTT_MID"))
        if valid(mid) then mid:SetVectorParameterValue(FName("Color"), { R = r, G = g, B = b, A = 1 }) end
    end)
    return a
end

local function textActor(text, size, r, g, b)
    local a = spawnActor("/Script/Engine.TextRenderActor")
    if not a then return nil end
    local tc = a.TextRender
    tc:SetMobility(2)
    tc:K2_SetText(FText(text))
    tc:SetWorldSize(size)
    tc:SetHorizontalAlignment(1)          -- EHTA_Center
    tc:SetTextRenderColor({ R = r, G = g, B = b, A = 255 })
    return a
end

local function destroyAvatar()
    if avatar then
        for _, a in pairs(avatar) do
            if valid(a) then pcall(function() a:K2_DestroyActor() end) end
        end
    end
    avatar = nil
end

local function ensureAvatar(name)
    if avatar and valid(avatar.head) and valid(avatar.left) and valid(avatar.right) then return true end
    destroyAvatar()   -- level change destroyed some parts: rebuild cleanly
    local ok, err = pcall(function()
        avatar = {
            head = meshActor("/Engine/BasicShapes/Sphere.Sphere", 0.20, 0.17, 0.24, 0.55, 0.05, 0.05),
            visor = meshActor("/Engine/BasicShapes/Cube.Cube", 0.05, 0.16, 0.06, 0.02, 0.02, 0.02),
            left = meshActor("/Engine/BasicShapes/Cube.Cube", 0.14, 0.08, 0.04, 0.8, 0.75, 0.7),
            right = meshActor("/Engine/BasicShapes/Cube.Cube", 0.14, 0.08, 0.04, 0.8, 0.75, 0.7),
            tag = textActor(name or "Player", 10, 230, 60, 60),
        }
    end)
    if not ok or not (avatar and avatar.head and avatar.left and avatar.right) then
        log("could not create the remote avatar: " .. tostring(err))
        destroyAvatar()
        return false
    end
    log("remote avatar created")
    return true
end

local function place(a, x, y, z, qx, qy, qz, qw)
    if not valid(a) then return end
    local p, yw, r = quatToRot(qx, qy, qz, qw)
    a:K2_SetActorLocationAndRotation({ X = x, Y = y, Z = z }, { Pitch = p, Yaw = yw, Roll = r }, false, {}, true)
end

local function hideAvatar()
    if not avatar then return end
    for _, a in pairs(avatar) do
        if valid(a) then pcall(function() a:SetActorHiddenInGame(true) end) end
    end
    avatar.hidden = true
end

local lastTagName = nil
-- shapes=false: the full body model is shown, so only the name tag is used from this avatar.
local function updateAvatar(v, peerName, quality, rtt, shapes)
    if avatarDisabled then return end
    if not ensureAvatar(peerName) then
        avatarDisabled = true
        return
    end
    if avatar.hidden or avatar.shapes ~= shapes then
        for k, a in pairs(avatar) do
            if k ~= "hidden" and k ~= "shapes" and valid(a) then a:SetActorHiddenInGame(not shapes and k ~= "tag") end
        end
        avatar.hidden = false
        avatar.shapes = shapes
    end
    if shapes then
        place(avatar.head, v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        local fx, fy, fz = qrot(v[4], v[5], v[6], v[7], 9, 0, 0)       -- visor 9 cm in front of the head centre
        place(avatar.visor, v[1] + fx, v[2] + fy, v[3] + fz, v[4], v[5], v[6], v[7])
        place(avatar.left, v[8], v[9], v[10], v[11], v[12], v[13], v[14])
        place(avatar.right, v[15], v[16], v[17], v[18], v[19], v[20], v[21])
    end
    -- name tag floats above the head and faces the local player
    if valid(avatar.tag) then
        local yaw = math.atan(localHead[2] - v[2], localHead[1] - v[1]) * RAD
        avatar.tag:K2_SetActorLocationAndRotation({ X = v[1], Y = v[2], Z = v[3] + 28 }, { Pitch = 0, Yaw = yaw, Roll = 0 }, false, {}, true)
        local label = string.format("%s  %dms", peerName, rtt)
        if label ~= lastTagName and frameNo % 45 == 0 then
            lastTagName = label
            avatar.tag.TextRender:K2_SetText(FText(label))
            local col = quality >= 3 and { R = 120, G = 230, B = 120, A = 255 } or quality == 2 and { R = 240, G = 200, B = 80, A = 255 } or { R = 230, G = 70, B = 60, A = 255 }
            avatar.tag.TextRender:SetTextRenderColor(col)
        end
    end
end

------------------------------------------------------------------------------- notifications in VR
local note = nil
local noteUntil = 0
local function showNote(text)
    log("note: " .. text)
    pcall(function()
        if not valid(note) then note = textActor(text, 6, 235, 225, 215) end
        if valid(note) then
            note.TextRender:K2_SetText(FText(text))
            note:SetActorHiddenInGame(false)
        end
    end)
    noteUntil = frameNo + 90 * 4
end

local headQuat = { 0, 0, 0, 1 }
local function updateNote()
    if not valid(note) then return end
    if frameNo > noteUntil then
        note:SetActorHiddenInGame(true)
        return
    end
    local fx, fy, fz = qrot(headQuat[1], headQuat[2], headQuat[3], headQuat[4], 70, 0, -12)
    local p, y, _ = quatToRot(headQuat[1], headQuat[2], headQuat[3], headQuat[4])
    note:K2_SetActorLocationAndRotation({ X = localHead[1] + fx, Y = localHead[2] + fy, Z = localHead[3] + fz },
        { Pitch = 0, Yaw = y + 180, Roll = 0 }, false, {}, true)
end

------------------------------------------------------------------------------- shared sounds
local pendingSounds = {}
local playingRemote = false
local shareRadius2 = 180 * 180

local function nearLocalPlayer(x, y, z)
    return dist2(x, y, z, localHead[1], localHead[2], localHead[3]) < shareRadius2
        or dist2(x, y, z, localLeft[1], localLeft[2], localLeft[3]) < shareRadius2
        or dist2(x, y, z, localRight[1], localRight[2], localRight[3]) < shareRadius2
end

local hookCalls, soundsCaptured, soundsPlayed, soundsMissing = 0, 0, 0, 0

local function captureSound(sound, x, y, z, vol, pitch)
    hookCalls = hookCalls + 1
    if playingRemote or #pendingSounds > 16 then return end
    if not valid(sound) or not nearLocalPlayer(x, y, z) then return end
    local path = objPath(sound)
    -- NPC sounds (footsteps, voices, gore) happen in both games already; only share the player's own.
    if path:find("/NPC/", 1, true) or path:find("_NPC_", 1, true) or path:find("/characters/", 1, true) then return end
    soundsCaptured = soundsCaptured + 1
    pendingSounds[#pendingSounds + 1] = string.format("S %s %.1f %.1f %.1f %.3f %.3f", objPath(sound), x, y, z, vol or 1, pitch or 1)
end

local function p(param) -- unwrap a hook parameter
    local ok, v = pcall(function() return param:get() end)
    if ok then return v end
    return nil
end

local function installSoundHooks()
    local hooks = {
        { "/Script/Engine.GameplayStatics:PlaySoundAtLocation", function(_, _, Sound, Location, _, Vol, Pitch)
            local l = p(Location)
            if l then captureSound(p(Sound), l.X, l.Y, l.Z, p(Vol), p(Pitch)) end
        end },
        { "/Script/Engine.GameplayStatics:SpawnSoundAtLocation", function(_, _, Sound, Location, _, Vol, Pitch)
            local l = p(Location)
            if l then captureSound(p(Sound), l.X, l.Y, l.Z, p(Vol), p(Pitch)) end
        end },
        { "/Script/Engine.GameplayStatics:SpawnSoundAttached", function(_, Sound, AttachTo, _, Location, _, LocType, _, Vol, Pitch)
            local comp = p(AttachTo)
            if valid(comp) then
                local l = comp:K2_GetComponentLocation()
                captureSound(p(Sound), l.X, l.Y, l.Z, p(Vol), p(Pitch))
            end
        end },
    }
    for _, h in ipairs(hooks) do
        local ok, err = pcall(RegisterHook, h[1], h[2])
        log((ok and "hooked " or "could not hook ") .. h[1] .. (ok and "" or (": " .. tostring(err))))
    end
end

local function playRemoteSound(path, x, y, z, vol, pitch)
    local snd = findAsset(path)
    if not snd then
        soundsMissing = soundsMissing + 1
        if soundsMissing <= 5 then log("remote sound asset not found: " .. path) end
        return
    end
    local world = UEHelpers.GetWorld()
    local gs = UEHelpers.GetGameplayStatics()
    playingRemote = true
    local ok, err = pcall(function()
        gs:PlaySoundAtLocation(world, snd, { X = x, Y = y, Z = z }, { Pitch = 0, Yaw = 0, Roll = 0 }, vol, pitch, 0, nil, nil, nil)
    end)
    playingRemote = false
    if ok then
        soundsPlayed = soundsPlayed + 1
        if soundsPlayed <= 3 then log(string.format("played remote sound %s at (%.0f, %.0f, %.0f)", path, x, y, z)) end
    else
        log("playing remote sound failed: " .. tostring(err))
    end
end

------------------------------------------------------------------------------- map following
local currentMap = nil
local travelling = false
local function travelTo(mapPath)
    if mapPath == currentMap then return end
    log("following host to " .. mapPath)
    showNote("Following host...")
    travelling = true
    pcall(function()
        local gs = UEHelpers.GetGameplayStatics()
        gs:OpenLevel(UEHelpers.GetWorld(), FName(mapPath), true, "")
    end)
end

------------------------------------------------------------------------------- frame
------------------------------------------------------------------------------- remote player body + combat
-- The other player is represented by the game's own base NPC (BP_NPC_Man) with its AI disabled.
-- Because it is a real NPC, your guns hit it with the game's normal damage/gore pipeline. Each hit
-- is forwarded to the other player's game, which applies it to their real character. That player's
-- game is authoritative for their health; when they die, their body ragdolls here.
local status = { state = "Idle" }
local NPC_PKG = "/Game/characters/Human/Blueprints/CharacterLogic/BP_NPC_Man"
local NPC_CLASS = NPC_PKG .. ".BP_NPC_Man_C"
local puppet = nil           -- the remote player's body actor
local puppetFailed = false
local puppetDead = false
local puppetLast = nil       -- last body position (for walk animation velocity)
local hitHookInstalled = false
local outbox = {}            -- "X ..." game event lines to send this frame
local lastHitFrame = {}      -- hit cooldowns
local puppetDownSince = nil  -- frame when the body got knocked into ragdoll while its owner lives
local selfTest = ""
local function sendGame(line) outbox[#outbox + 1] = "X " .. line end

local function damageForHit(bone, causer)
    bone = (bone or ""):lower()
    local dmg = 20
    if bone:find("head") or bone:find("neck") or bone:find("facial") or bone:find("jaw") then
        dmg = 100
    elseif bone:find("spine") or bone:find("pelvis") or bone:find("clavicle") or bone:find("rib") then
        dmg = 40
    end
    causer = (causer or ""):lower()
    if causer:find("melee", 1, true) then
        return (dmg >= 100) and 25 or 10  -- punches / bats / knives: several hits needed
    end
    if causer:find("50bmg") or causer:find("12ga") or causer:find("308") or causer:find("44mag") or causer:find("50ae") then
        dmg = dmg * 1.5
    end
    return dmg
end

-- Same call the game's projectile (BP_BT_Projectile_Base) makes when a bullet hits an NPC:
-- EventReceiveDamageNPC(HitResult, <row of BT_DamageCauser_DataTable>, CauserID, ExitWound, PhysHitReact).
local DAMAGE_TABLE = "/Game/BloodTrailWeapons/Data/DataTables/BT_DamageCauser_DataTable.BT_DamageCauser_DataTable"
local causerRows = nil
local function causerRow(id)
    if not causerRows then
        causerRows = {}
        local dt = StaticFindObject(DAMAGE_TABLE)
        if not valid(dt) then
            pcall(LoadAsset, DAMAGE_TABLE:match("^(.-)%."))
            dt = StaticFindObject(DAMAGE_TABLE)
        end
        if valid(dt) then dt:ForEachRow(function(name, row) causerRows[tostring(name)] = row end) end
    end
    return causerRows[id]
end

local function applyBulletHit(npc, bone, x, y, z, nx, ny, nz, causer, exitWound)
    local row = causerRow(causer)
    if not row then return false, "unknown damage causer " .. tostring(causer) end
    local hit = {
        bBlockingHit = true,
        Location = { X = x, Y = y, Z = z }, ImpactPoint = { X = x, Y = y, Z = z },
        Normal = { X = nx, Y = ny, Z = nz }, ImpactNormal = { X = nx, Y = ny, Z = nz },
        TraceStart = { X = x + nx * 100, Y = y + ny * 100, Z = z + nz * 100 }, TraceEnd = { X = x - nx * 50, Y = y - ny * 50, Z = z - nz * 50 },
        BoneName = FName(bone),
    }
    npc:EventReceiveDamageNPC(hit, row, FName(causer), exitWound and true or false, true)
    return true
end

local function isPuppet(actor)
    return valid(puppet) and valid(actor) and actor:GetFullName() == puppet:GetFullName()
end

local function installHitHook()
    if hitHookInstalled then return end
    hitHookInstalled = true
    local ok, err = pcall(RegisterHook, NPC_CLASS .. ":EventReceiveDamageNPC",
        function(ctx, HitResult, DamageCauserStruct, DamageCauserID, ExitWound)
            local npc = ctx:get()
            if not isPuppet(npc) or puppetDead then return end
            local bone, causer, hx, hy, hz = "", "", nil, nil, nil
            pcall(function()
                local hr = p(HitResult)
                bone = hr.BoneName:ToString()
                hx, hy, hz = hr.ImpactPoint.X, hr.ImpactPoint.Y, hr.ImpactPoint.Z
            end)
            pcall(function() causer = p(DamageCauserID):ToString() end)
            if p(ExitWound) then return end  -- the entry wound already counted
            -- Only damage dealt by YOU counts: bullets (only players and NPC guns fire them; NPCs don't
            -- target this body) and melee only when one of your hands is right at the impact point.
            -- This drops blunt "hits" from the body falling, bumping walls or ragdoll head-slams.
            if causer:find("melee", 1, true) then
                local nearHand = hx and (dist2(hx, hy, hz, localLeft[1], localLeft[2], localLeft[3]) < 60 * 60 or
                                         dist2(hx, hy, hz, localRight[1], localRight[2], localRight[3]) < 60 * 60)
                if not nearHand then return end
            elseif not causer:find("ammo", 1, true) and causer ~= "" then
                return  -- explosions etc. are not attributed yet
            end
            -- Bodies touching each other register as blunt "melee" hits every physics frame:
            -- allow at most one hit per causer+bone every 0.5 s (and blunt hits once per 1.5 s).
            local key = causer .. "|" .. bone
            local cooldown = causer:find("melee_blunt", 1, true) and 135 or 45
            if lastHitFrame[key] and frameNo - lastHitFrame[key] < cooldown then return end
            lastHitFrame[key] = frameNo
            local dmg = damageForHit(bone, causer)
            log(string.format("hit the other player: %s with %s -> %d damage", bone, causer, dmg))
            sendGame(string.format("hit %d %s %s", dmg, bone ~= "" and bone or "-", causer ~= "" and causer or "-"))
        end)
    log((ok and "hooked " or "could not hook ") .. "EventReceiveDamageNPC" .. (ok and "" or (": " .. tostring(err))))
end

local function destroyPuppet()
    if valid(puppet) then pcall(function() puppet:K2_DestroyActor() end) end
    puppet, puppetLast = nil, nil
end

local function spawnPuppet(x, y, z, yaw)
    pcall(LoadAsset, NPC_PKG)
    local cls = StaticFindObject(NPC_CLASS)
    local world = UEHelpers.GetWorld()
    local gs = UEHelpers.GetGameplayStatics()
    if not (valid(cls) and valid(world) and valid(gs)) then return nil end
    local qx, qy, qz, qw = rotToQuat(0, yaw, 0)
    local t = { Rotation = { X = qx, Y = qy, Z = qz, W = qw }, Translation = { X = x, Y = y, Z = z }, Scale3D = { X = 1, Y = 1, Z = 1 } }
    local a = gs:BeginDeferredActorSpawnFromClass(world, cls, t, 1, nil)
    if not valid(a) then return nil end
    pcall(function() a.AutoPossessAI = 0 end)        -- EAutoPossessAI::Disabled: no brain
    gs:FinishSpawningActor(a, t)
    if not valid(a) then return nil end
    pcall(function() a:DetachFromControllerPendingDestroy() end)
    -- Stay in "walking" mode (so the animation does not play a fall) but stop the movement
    -- component from simulating: we place the body ourselves every frame.
    pcall(function() a.CharacterMovement:SetMovementMode(1, 0) end)
    pcall(function() a.CharacterMovement:SetComponentTickEnabled(false) end)
    pcall(function() a.CharacterMovement.GravityScale = 0 end)
    pcall(function() a.CapsuleComponent:SetCollisionEnabled(0) end)  -- don't block you; the mesh still takes bullets
    pcall(function() a.HealthOverall = 100000 end)                   -- never dies locally; the owner decides
    pcall(function() a.DebugTextNPC:SetHiddenInGame(true, false) end)
    pcall(function() a.DebugTextLocal:SetHiddenInGame(true, false) end)
    installHitHook()
    log("remote player body spawned: " .. a:GetFullName())
    return a
end

local function half_default()
    local h = 90
    pcall(function() h = puppet.CapsuleComponent:GetScaledCapsuleHalfHeight() end)
    return h
end

-- v: head(1-7) left(8-14) right(15-21) body(22-28)
local function updatePuppet(v)
    if puppetFailed then return false end
    if not valid(puppet) then
        if puppet ~= nil then log("remote player body disappeared (destroyed by the game?) - respawning") puppet = nil end
        if puppetDead then return true end
        local ok, a = pcall(spawnPuppet, v[22], v[23], v[24] + 90, select(2, quatToRot(v[25], v[26], v[27], v[28])))
        if not ok or not a then
            log("could not spawn the remote player body (" .. tostring(a) .. "), using simple shapes")
            puppetFailed = true
            return false
        end
        puppet = a
    end
    if puppetDead then return true end
    -- Hit reactions can knock the body into ragdoll. Its owner is still alive, so get it back up
    -- (the game's own get-up via RagdollEnd keeps the wounds); force it if that does not work.
    local okS, sim = pcall(function()
        return puppet.Mesh:IsSimulatingPhysics(FName("Pelvis")) or puppet.Mesh:IsSimulatingPhysics(FName("None"))
    end)
    if not okS and frameNo % 900 == 0 then log("ragdoll check failed: " .. tostring(sim)) end
    if okS and sim then
        puppetDownSince = puppetDownSince or frameNo
        local down = frameNo - puppetDownSince
        if down == 60 then
            log("remote body knocked down - getting it back up")
            local okE, errE = pcall(function() puppet:RagdollEnd() end)
            if not okE then log("RagdollEnd failed: " .. tostring(errE)) end
        end
        if down == 240 then
            log("remote body stayed down - resetting its physics")
            pcall(function()
                puppet.Mesh:SetSimulatePhysics(false)
                puppet.Mesh:K2_AttachToComponent(puppet.CapsuleComponent, FName("None"), 2, 2, 2, true)
                puppet.Mesh:K2_SetRelativeLocationAndRotation({ X = 0, Y = 0, Z = -half_default() }, { Pitch = 0, Yaw = -90, Roll = 0 }, false, {}, true)
            end)
        end
        if down < 240 then return true end  -- leave the physics alone while it falls / gets up
    else
        puppetDownSince = nil
    end
    local half = 90
    pcall(function() half = puppet.CapsuleComponent:GetScaledCapsuleHalfHeight() end)
    local yaw = select(2, quatToRot(v[25], v[26], v[27], v[28]))
    -- keep the NPC's head under the real head: the head sits ~10 cm in front of the capsule centre
    local fx, fy = math.cos(yaw * DEG), math.sin(yaw * DEG)
    local x, y, z = v[1] - fx * 10, v[2] - fy * 10, v[24] + half
    puppet:K2_SetActorLocationAndRotation({ X = x, Y = y, Z = z }, { Pitch = 0, Yaw = yaw, Roll = 0 }, false, {}, true)
    -- drive the walk/idle animation with the real movement speed
    if puppetLast then
        local vx, vy = (x - puppetLast[1]) * 90, (y - puppetLast[2]) * 90
        pcall(function() puppet.CharacterMovement.Velocity = { X = vx, Y = vy, Z = 0 } end)
    end
    puppetLast = { x, y }
    -- hands: the NPC rig has IK targets for both hands
    pcall(function()
        local lp, ly, lr = quatToRot(v[11], v[12], v[13], v[14])
        local rp, ry, rr = quatToRot(v[18], v[19], v[20], v[21])
        puppet.ik_hand_l:K2_SetWorldLocationAndRotation({ X = v[8], Y = v[9], Z = v[10] }, { Pitch = lp, Yaw = ly, Roll = lr }, false, {}, true)
        puppet.ik_hand_r:K2_SetWorldLocationAndRotation({ X = v[15], Y = v[16], Z = v[17] }, { Pitch = rp, Yaw = ry, Roll = rr }, false, {}, true)
    end)
    return true
end

local function puppetDie()
    if puppetDead then return end
    puppetDead = true
    if valid(puppet) then
        log("other player died: ragdolling their body")
        local ok = pcall(function() puppet:RagdollStart() end)
        if not ok then pcall(function() puppet.Mesh:SetSimulatePhysics(true) end) end
    end
end

local function puppetRespawn()
    puppetDead = false
    destroyPuppet()
end

-- my own life, reported to the other player
local myDeadReported = false
local function watchMyHealth()
    local pawn = UEHelpers.GetPlayer()
    if not valid(pawn) then return end
    local okH, hp = pcall(function() return pawn.PlayerHealth end)
    if not okH or type(hp) ~= "number" then return end
    if hp <= 0 and not myDeadReported then
        myDeadReported = true
        sendGame("dead")
    elseif hp > 0 and myDeadReported then
        myDeadReported = false
        sendGame("alive")
    end
end

local function handleGame(line)
    local kind, rest = line:match("^(%S+)%s*(.*)$")
    if kind == "hit" then
        local dmg, bone, causer = rest:match("^(%S+) (%S+) (%S+)")
        dmg = tonumber(dmg) or 0
        local pawn = UEHelpers.GetPlayer()
        if valid(pawn) and dmg > 0 then
            log(string.format("shot by the other player: %s (%s), %d damage", bone or "?", causer or "?", dmg))
            local ok, err = pcall(function() pawn:BulletDamage(dmg) end)
            if not ok then log("applying damage failed: " .. tostring(err)) end
        end
    elseif kind == "dead" then
        puppetDie()
        showNote((status.peer or "The other player") .. " died")
    elseif kind == "alive" then
        puppetRespawn()
    end
end

local handshakeDone = false
local framePending = false

local function parseReply(reply)
    for line in reply:gmatch("[^\n]+") do
        local kind = line:sub(1, 2)
        if kind == "ST" then
            local st, role, q, rtt, loss, sim, code, peer = line:match("^ST (%S+) (%S+) (%d+) (%S+) (%S+) (%d) (%S+) (%S+)")
            if st then
                status.state, status.role, status.quality, status.rtt = st, role, tonumber(q), tonumber(rtt) or 0
                status.peer = (peer ~= "-") and peer or "Player"
                status.code = code
            end
        elseif kind == "RP" then
            if line == "RP -" then
                hideAvatar()
                if valid(puppet) and not puppetDead then destroyPuppet() end
            else
                local v = {}
                for num in line:sub(4):gmatch("%S+") do v[#v + 1] = tonumber(num) end
                if #v >= 28 then
                    local okP, body = pcall(updatePuppet, v)
                    if not okP then
                        log("body update failed: " .. tostring(body) .. " - falling back to simple shapes")
                        puppetFailed = true
                        destroyPuppet()
                        body = false
                    end
                    local ok, err = pcall(updateAvatar, v, status.peer or "Player", status.quality or 0, math.floor(status.rtt or 0), not body)
                    if not ok then log("avatar update failed: " .. tostring(err)) avatarDisabled = true end
                end
            end
        elseif kind == "GX" then
            local ok, err = pcall(handleGame, line:sub(4))
            if not ok then log("game event failed: " .. tostring(err)) end
        elseif kind == "SN" then
            local path, x, y, z, vol, pitch = line:match("^SN (%S+) (%S+) (%S+) (%S+) (%S+) (%S+)")
            if path then playRemoteSound(path, tonumber(x), tonumber(y), tonumber(z), tonumber(vol), tonumber(pitch)) end
        elseif kind == "TR" then
            travelTo(line:sub(4))
        elseif kind == "NT" then
            showNote(line:sub(4))
        elseif kind == "CF" then
            local radius, _, st = line:match("^CFG (%S+) (%S+) ?(%S*)")
            if radius then shareRadius2 = tonumber(radius) ^ 2 end
            if st and st ~= "" and st ~= "-" then selfTest = st log("developer self-test: " .. st) end
        end
    end
end

local function frame()
    framePending = false
    frameNo = frameNo + 1
    if not pipeUp then
        if frameNo % 90 == 1 and bridgeFailures < 50 then
            if openBridge() then handshakeDone = false end
        end
        if not pipeUp then return end
    end

    local req = {}
    if not handshakeDone then
        req[#req + 1] = "H " .. VERSION
        handshakeDone = true
    end
    local okPose, poseLine = pcall(readLocalPose)
    if okPose and poseLine then req[#req + 1] = poseLine end
    pcall(function()
        local pc = UEHelpers.GetPlayerController()
        if valid(pc) and valid(pc.PlayerCameraManager) then
            local r = pc.PlayerCameraManager:GetCameraRotation()
            headQuat[1], headQuat[2], headQuat[3], headQuat[4] = rotToQuat(r.Pitch, r.Yaw, r.Roll)
        end
    end)
    if frameNo % MAP_POLL_FRAMES == 0 or currentMap == nil then
        local okM, m = pcall(currentMapPath)
        if okM and m and m ~= currentMap then
            currentMap = m
            travelling = false
            avatarDisabled = false   -- try again on the new map
            destroyAvatar()
            note = nil
            puppet, puppetLast, puppetDead, puppetFailed = nil, nil, false, false  -- old body died with the old map
        end
        if currentMap then req[#req + 1] = "M " .. currentMap end
    end
    if frameNo % 900 == 0 then
        req[#req + 1] = string.format("D sound hooks fired=%d captured=%d played=%d missing=%d avatar=%s",
            hookCalls, soundsCaptured, soundsPlayed, soundsMissing, tostring(avatar ~= nil and not avatarDisabled))
    end
    for i = 1, #pendingSounds do req[#req + 1] = pendingSounds[i] end
    pendingSounds = {}
    pcall(watchMyHealth)
    -- developer self-test: shoot the other player's body with a point-damage event
    if selfTest == "hit" and valid(puppet) and frameNo % 600 == 0 then
        local okT, errT = pcall(function()
            -- a 9mm round into the chest, coming from the local player's direction
            local hl = puppet.Mesh:GetSocketLocation(FName("spine_03"))
            local dx, dy = localHead[1] - hl.X, localHead[2] - hl.Y
            local len = math.max(1, math.sqrt(dx * dx + dy * dy))
            local ok, why = applyBulletHit(puppet, "spine_03", hl.X, hl.Y, hl.Z, dx / len, dy / len, 0, "ammo_9mm_b", false)
            log("self-test: bullet into the remote body: " .. tostring(ok) .. " " .. tostring(why or ""))
        end)
        if not okT then log("self-test failed: " .. tostring(errT)) end
    end
    for i = 1, #outbox do req[#req + 1] = outbox[i] end
    outbox = {}

    local reply = exchange(table.concat(req, "\n") .. "\n")
    if reply then
        local ok, err = pcall(parseReply, reply)
        if not ok then log("reply handling failed: " .. tostring(err)) end
    end
    pcall(updateNote)
end

------------------------------------------------------------------------------- start
installSoundHooks()

RegisterKeyBind(KEY_TOGGLE_WINDOW, function()
    if pipeUp then exchange("K toggle_window\n") end
end)

local function safeFrame()
    local ok, err = pcall(frame)
    if not ok then log("frame error: " .. tostring(err)) end
end

if type(LoopInGameThreadAfterFrames) == "function" then
    -- One persistent callback, run by UE4SS on the game thread every frame. (Creating a new
    -- ExecuteInGameThread closure ~90 times a second from LoopAsync crashed UE4SS in testing:
    -- "Ref was not function" followed by an access violation.)
    LoopInGameThreadAfterFrames(1, function()
        safeFrame()
        return false
    end)
    log("frame loop: LoopInGameThreadAfterFrames")
else
    LoopAsync(FRAME_MS, function()
        if not framePending then
            framePending = true
            ExecuteInGameThread(safeFrame)
        end
        return false  -- keep looping
    end)
    log("frame loop: LoopAsync + ExecuteInGameThread (older UE4SS)")
end

log("BloodTrail Together " .. VERSION .. " Lua side started (F9 toggles the multiplayer window)")
