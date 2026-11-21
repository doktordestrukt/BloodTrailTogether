-- BloodTrail Together - UE4SS Lua side (game thread).
-- Responsibilities:
--   * load bin\btt_core.dll (networking, Unity Relay, UI window) with package.loadlib(path, "*")
--   * every frame: read the local VR pose (HMD + both motion controllers, world space) and the
--     current map, exchange one frame with the DLL over a named pipe, then draw the remote avatar,
--     play shared sounds, follow the host's map and show notifications.
-- Everything that touches the game is wrapped in pcall: a failure disables that feature and is
-- logged to UE4SS.log / BloodTrailTogether.log instead of crashing the game.

local VERSION = "0.4.0"
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

-- PERFORMANCE: UEHelpers.GetPlayerController()/GetPlayer()/GetWorld() each scan EVERY object in the
-- game (FindAllOf). Calling them several times per frame was the main cause of the frame-rate
-- drops in v0.2.x. The controller is cached and only looked up again when it becomes invalid
-- (map change), at most twice a second.
local cachedPC, lastPCSearch = nil, -10
local function getPC()
    if valid(cachedPC) then return cachedPC end
    local now = os.clock()
    if now - lastPCSearch < 0.5 then return nil end
    lastPCSearch = now
    local pc = UEHelpers.GetPlayerController()
    if valid(pc) then cachedPC = pc return pc end
    return nil
end
local function getPawn()
    local pc = getPC()
    if not pc then return nil end
    local ok, pawn = pcall(function() return pc.Pawn end)
    if ok and valid(pawn) then return pawn end
    return nil
end
local function getWorld()
    local pc = getPC()
    if pc then
        local ok, w = pcall(function() return pc:GetWorld() end)
        if ok and valid(w) then return w end
    end
    return nil
end

local function currentMapPath()
    local world = getWorld()
    if not valid(world) then return nil end
    local p = objPath(world)                 -- /Game/Maps/Sandbox/Map_Sandbox.Map_Sandbox
    return p:match("^(.-)%.[^/]+$") or p     -- /Game/Maps/Sandbox/Map_Sandbox
end

local assetCache = {}
local function findAsset(path)
    local o = assetCache[path]
    if valid(o) then return o end
    o = StaticFindObject(path)
    -- load from disk at most once per path: retrying a failing load every frame stalled and
    -- then crashed the game in testing
    if not valid(o) and o ~= false and assetCache[path] ~= false then
        pcall(LoadAsset, path:match("^(.-)%.") or path)
        o = StaticFindObject(path)
    end
    if valid(o) then assetCache[path] = o return o end
    assetCache[path] = false
    return nil
end

------------------------------------------------------------------------------- quaternion helpers ({x,y,z,w})
local function qmul(a, b)
    return { a[4] * b[1] + a[1] * b[4] + a[2] * b[3] - a[3] * b[2],
             a[4] * b[2] - a[1] * b[3] + a[2] * b[4] + a[3] * b[1],
             a[4] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[4],
             a[4] * b[4] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3] }
end
local function qconj(q) return { -q[1], -q[2], -q[3], q[4] } end
local function qnorm(q)
    local n = math.sqrt(q[1] * q[1] + q[2] * q[2] + q[3] * q[3] + q[4] * q[4])
    if n < 1e-9 then return { 0, 0, 0, 1 } end
    return { q[1] / n, q[2] / n, q[3] / n, q[4] / n }
end
-- shortest rotation that turns direction u into direction v
local function fromTo(ux, uy, uz, vx, vy, vz)
    local lu = math.sqrt(ux * ux + uy * uy + uz * uz)
    local lv = math.sqrt(vx * vx + vy * vy + vz * vz)
    if lu < 1e-6 or lv < 1e-6 then return { 0, 0, 0, 1 } end
    ux, uy, uz, vx, vy, vz = ux / lu, uy / lu, uz / lu, vx / lv, vy / lv, vz / lv
    local d = ux * vx + uy * vy + uz * vz
    if d < -0.9999 then
        local ax, ay, az = 0, -uz, uy
        if ay * ay + az * az < 1e-6 then ax, ay, az = -uz, 0, ux end
        return qnorm({ ax, ay, az, 0 })
    end
    return qnorm({ uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx, 1 + d })
end
local function qv(q, x, y, z) return qrot(q[1], q[2], q[3], q[4], x, y, z) end

------------------------------------------------------------------------------- local VR pose
local controllers = { left = nil, right = nil }
local frameNo = 0
local headQuat = { 0, 0, 0, 1 }

local function scanControllers()
    controllers.left, controllers.right = nil, nil
    -- Normal case: the VRExpansion pawn's own grip controllers (no object scan needed).
    local pawn = getPawn()
    if pawn then
        pcall(function()
            local l, r = pawn["Left Grip Motion Controller"], pawn["Right Grip Motion Controller"]
            if valid(l) then controllers.left = l end
            if valid(r) then controllers.right = r end
        end)
        if controllers.left and controllers.right then return end
    end
    -- Other pawns: search motion controller components (rare, throttled by the caller).
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

-- What each hand holds: the mesh of the gripped object (gun, magazine, knife...), streamed with
-- its exact world transform so the other player sees it in the right hand, at the right angle,
-- including reloads (the magazine in the other hand).
local heldState = { L = { name = nil, comp = nil }, R = { name = nil, comp = nil } }
local heldEvents = {}   -- "held ..." / "heldpart ..." lines to send
local vrBaseClass = nil
-- v0.3 extras (torch / night vision, grabbing and throwing bodies and props). One table instead of
-- many locals: Lua allows at most 200 locals per function and this file's main chunk is close.
local EX = { grab = {}, dropAt = {}, remoteGrab = {}, remoteVis = { lamp = 0, nv = 0 }, props = {}, propCache = {}, levelOf = {} }

-- Objects that are part of the map (loaded with it, not spawned at runtime) exist in both games
-- under the same name.
function EX.isLevel(a)
    local path = objPath(a)
    local r = EX.levelOf[path]
    if r == nil then
        local ok, v = pcall(function()
            local f = (EObjectFlags and EObjectFlags.RF_WasLoaded) or 0x80000
            return a:HasAnyFlags(f)
        end)
        r = ok and v == true
        EX.levelOf[path] = r
    end
    return r
end

-- K2_GetComponentsByClass returns a Lua table or a TArray depending on the UE4SS build
local function componentsOf(actor, cls)
    local out = {}
    local ok, comps = pcall(function() return actor:K2_GetComponentsByClass(cls) end)
    if not ok or not comps then return out end
    if type(comps) == "table" then
        for _, c in ipairs(comps) do out[#out + 1] = c end
    else
        pcall(function() comps:ForEach(function(_, e) out[#out + 1] = e:get() end) end)
    end
    return out
end

local NPC_CLASS_PATH = "/Game/characters/Human/Blueprints/CharacterLogic/BP_NPC_Man.BP_NPC_Man_C"
local CLS = {}
local function cls(path)
    local c = CLS[path]
    if valid(c) then return c end
    c = StaticFindObject(path)
    CLS[path] = c
    return c
end

-- Every visible mesh of the held thing (receiver, slide, pump, magazine, scope, attached flashlight...)
-- with its transform relative to the gripped component. Returns the part lines and a signature.
local function heldParts(actor, main)
    local parts, sig = {}, {}
    local mt = main:K2_GetComponentToWorld()
    local mq = { mt.Rotation.X, mt.Rotation.Y, mt.Rotation.Z, mt.Rotation.W }
    local ms = (mt.Scale3D.X ~= 0) and mt.Scale3D.X or 1
    local iq = qconj(mq)
    local function add(c, kind)
        if #parts >= 12 or not valid(c) then return end
        local okV, vis = pcall(function() return c:IsVisible() and not c.bHiddenInGame end)
        if not okV or not vis then return end
        local mesh = kind == "K" and c.SkeletalMesh or c.StaticMesh
        if not valid(mesh) then return end
        local t = c:K2_GetComponentToWorld()
        local rx, ry, rz = qv(iq, t.Translation.X - mt.Translation.X, t.Translation.Y - mt.Translation.Y, t.Translation.Z - mt.Translation.Z)
        local rq = qmul(iq, { t.Rotation.X, t.Rotation.Y, t.Rotation.Z, t.Rotation.W })
        local path = objPath(mesh)
        parts[#parts + 1] = string.format("%s %s %.2f %.2f %.2f %.4f %.4f %.4f %.4f %.3f", path, kind,
            rx / ms, ry / ms, rz / ms, rq[1], rq[2], rq[3], rq[4], t.Scale3D.X / ms)
        sig[#sig + 1] = path
    end
    local function addActor(a)
        for _, c in ipairs(componentsOf(a, cls("/Script/Engine.SkeletalMeshComponent"))) do add(c, "K") end
        for _, c in ipairs(componentsOf(a, cls("/Script/Engine.StaticMeshComponent"))) do add(c, "S") end
    end
    addActor(actor)
    -- magazines, lights, suppressors... that are separate actors attached to the gun
    pcall(function()
        local att = {}
        actor:GetAttachedActors(att)
        for _, a in ipairs(att) do
            local aa = type(a) == "userdata" and a.get and a:get() or a
            if valid(aa) then addActor(aa) end
        end
    end)
    return parts, table.concat(sig, ","), ms
end

local function resolveHeld(side, ctrl)
    local st = heldState[side]
    local obj = nil
    pcall(function()
        local arr = ctrl.GrippedObjects
        if arr:GetArrayNum() > 0 then obj = arr[1].GrippedObject end
    end)
    local name = valid(obj) and obj:GetFullName() or nil
    -- re-read the parts twice a second while held: a magazine coming out / going in shows up
    if name ~= st.name or (name and frameNo % 45 == 0) then
        if name ~= st.name then st.name, st.comp, st.actor, st.sig = name, nil, nil, nil end
        local parts, sig, ms = {}, "", 1
        if name then
            pcall(function()
                local actor, main = obj, nil
                if obj:IsA(cls("/Script/Engine.Actor")) then
                    main = obj:K2_GetRootComponent()
                else
                    actor, main = obj:GetOwner(), obj
                end
                -- an NPC body being dragged is not drawn as a "held item" (the body itself is synced)
                if actor:IsA(cls(NPC_CLASS_PATH)) then return end
                -- a map object exists in the other game too: the real one is moved there (prop sync)
                if EX.isLevel(actor) then st.comp, st.actor = main, actor return end
                parts, sig, ms = heldParts(actor, main)
                st.comp, st.actor = main, actor
            end)
        end
        if sig ~= st.sig then
            st.sig = sig
            heldEvents[#heldEvents + 1] = string.format("held %s %d %.3f", side, #parts, ms)
            for i, p in ipairs(parts) do heldEvents[#heldEvents + 1] = string.format("heldpart %s %d %s", side, i, p) end
            if sig ~= "" then log("holding in " .. side .. " hand: " .. sig) end
        end
    end
    if valid(st.comp) and st.sig and st.sig ~= "" then
        local t = st.comp:K2_GetComponentToWorld()
        return { t.Translation.X, t.Translation.Y, t.Translation.Z, t.Rotation.X, t.Rotation.Y, t.Rotation.Z, t.Rotation.W }
    end
    return nil
end

-- Returns the "P ..." request line or nil
local function readLocalPose()
    local pc = getPC()
    if not pc then return nil end
    local cam = pc.PlayerCameraManager
    if not valid(cam) then return nil end
    local hl = cam:GetCameraLocation()
    local hr = cam:GetCameraRotation()
    local hx, hy, hz = hl.X, hl.Y, hl.Z
    local qx, qy, qz, qw = rotToQuat(hr.Pitch, hr.Yaw, hr.Roll)
    localHead[1], localHead[2], localHead[3] = hx, hy, hz
    headQuat[1], headQuat[2], headQuat[3], headQuat[4] = qx, qy, qz, qw

    if (not valid(controllers.left) or not valid(controllers.right)) and frameNo % 120 == 1 then
        pcall(scanControllers)
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
    -- Body: floor point under the head. VRExpansion pawns have their actor ON the floor
    -- (measured: actor Z 0.6, floor 0); other pawns at the capsule centre.
    local floorZ = hz - 165
    local pawn = getPawn()
    if pawn then
        pcall(function()
            local pz = pawn:K2_GetActorLocation().Z
            if not vrBaseClass then vrBaseClass = StaticFindObject("/Script/VRExpansionPlugin.VRBaseCharacter") end
            if valid(vrBaseClass) and pawn:IsA(vrBaseClass) then
                floorZ = pz
            else
                local half = 0
                pcall(function() half = pawn.CapsuleComponent:GetScaledCapsuleHalfHeight() end)
                floorZ = pz - half
            end
        end)
    end
    -- held items
    local HL, HR = { 0, 0, 0, 0, 0, 0, 1 }, { 0, 0, 0, 0, 0, 0, 1 }
    if valid(controllers.left) then
        local ok, t = pcall(resolveHeld, "L", controllers.left)
        if ok and t then HL = t flags = flags | 4 end
    end
    if valid(controllers.right) then
        local ok, t = pcall(resolveHeld, "R", controllers.right)
        if ok and t then HR = t flags = flags | 8 end
    end
    local yaw = select(2, quatToRot(qx, qy, qz, qw))
    local bqx, bqy, bqz, bqw = rotToQuat(0, yaw, 0)
    local f7 = " %.2f %.2f %.2f %.5f %.5f %.5f %.5f"
    return string.format("P" .. f7 .. f7 .. f7 .. f7 .. f7 .. f7 .. " %d",
        hx, hy, hz, qx, qy, qz, qw, L[1], L[2], L[3], L[4], L[5], L[6], L[7], R[1], R[2], R[3], R[4], R[5], R[6], R[7],
        hx, hy, floorZ, bqx, bqy, bqz, bqw,
        HL[1], HL[2], HL[3], HL[4], HL[5], HL[6], HL[7], HR[1], HR[2], HR[3], HR[4], HR[5], HR[6], HR[7], flags)
end

------------------------------------------------------------------------------- remote avatar
local avatar = nil   -- { head, visor, left, right, tag }
local avatarDisabled = false
local IDENTITY = { Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Translation = { X = 0, Y = 0, Z = -100000 }, Scale3D = { X = 1, Y = 1, Z = 1 } }

local function spawnActor(classPath)
    local world = getWorld()
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
            -- health bar under the name tag (dark back plate + red fill)
            hpBg = meshActor("/Engine/BasicShapes/Cube.Cube", 0.004, 0.32, 0.035, 0.03, 0.03, 0.03),
            hpFg = meshActor("/Engine/BasicShapes/Cube.Cube", 0.006, 0.30, 0.025, 0.75, 0.05, 0.05),
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
local remoteHealth, shownHealth = 1, 1   -- other player's health share (from their "hp" events)
-- shapes=false: the full body model is shown, so only the name tag is used from this avatar.
local function updateAvatar(v, peerName, quality, rtt, shapes)
    if avatarDisabled then return end
    if not ensureAvatar(peerName) then
        avatarDisabled = true
        return
    end
    if avatar.hidden or avatar.shapes ~= shapes then
        for k, a in pairs(avatar) do
            if k ~= "hidden" and k ~= "shapes" and valid(a) then
                a:SetActorHiddenInGame(not shapes and k ~= "tag" and k ~= "hpBg" and k ~= "hpFg")
            end
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
        -- health bar: fills from the left, shrinks smoothly as the other player is hurt
        pcall(function()
            local frac = math.max(0, math.min(1, remoteHealth or 1))
            shownHealth = shownHealth + (frac - shownHealth) * 0.15
            local ry = yaw * DEG
            local rx, rY = -math.sin(ry), math.cos(ry)          -- the bar's left-right direction
            local bz = v[3] + 22
            avatar.hpBg:K2_SetActorLocationAndRotation({ X = v[1], Y = v[2], Z = bz }, { Pitch = 0, Yaw = yaw, Roll = 0 }, false, {}, true)
            local w = 0.30 * math.max(0.001, shownHealth)
            local shift = (0.30 - w) * 50                         -- keep the fill's left edge in place
            avatar.hpFg:K2_SetActorLocationAndRotation({ X = v[1] + rx * shift, Y = v[2] + rY * shift, Z = bz }, { Pitch = 0, Yaw = yaw, Roll = 0 }, false, {}, true)
            avatar.hpFg:SetActorScale3D({ X = 0.006, Y = w, Z = 0.025 })
        end)
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
    if path:find("/NPC/", 1, true) or path:find("_NPC_", 1, true) or path:find("/characters/", 1, true)
        or path:find("/blood/", 1, true) or path:find("Gore", 1, true) or path:find("/body_fall/", 1, true) then return end
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

-- The player's own gun sounds are made for the shooter: no 3D settings, so the other player heard
-- them "in his ear" at full volume. They are now played with the game's own gunshot attenuation
-- (the one its NPC guns use): direction and distance falloff. Other player sounds without 3D
-- settings (magazines, foley) get a short-range attenuation.
local ATT_GUN = "/Game/Audio/attenuations/ATT_gunshot.ATT_gunshot"
local ATT_NEAR = "/Game/Audio/attenuations/ATT_shelldrop.ATT_shelldrop"
local function attenuationFor(path, snd)
    local lp = path:lower()
    if lp:find("fire") or lp:find("shot") or lp:find("gun") or lp:find("blast") or lp:find("suppressed") then
        return findAsset(ATT_GUN)
    end
    local own = nil
    pcall(function() own = snd.AttenuationSettings end)
    if valid(own) then return nil end      -- already a 3D sound
    return findAsset(ATT_NEAR)
end

local function playRemoteSound(path, x, y, z, vol, pitch)
    local snd = findAsset(path)
    if not snd then
        soundsMissing = soundsMissing + 1
        if soundsMissing <= 5 then log("remote sound asset not found: " .. path) end
        return
    end
    local world = getWorld()
    local gs = UEHelpers.GetGameplayStatics()
    local att = nil
    pcall(function() att = attenuationFor(path, snd) end)
    playingRemote = true
    local ok, err = pcall(function()
        gs:PlaySoundAtLocation(world, snd, { X = x, Y = y, Z = z }, { Pitch = 0, Yaw = 0, Roll = 0 }, vol, pitch, 0, att, nil, nil)
    end)
    playingRemote = false
    if ok then
        soundsPlayed = soundsPlayed + 1
        if soundsPlayed <= 5 then log(string.format("played remote sound %s at (%.0f, %.0f, %.0f), 3D: %s", path, x, y, z, valid(att) and objPath(att) or "its own")) end
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
        gs:OpenLevel(getWorld(), FName(mapPath), true, "")
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

-- Damage as a SHARE of the victim's max health (their game knows its own health scale):
-- head = dead, chest/pelvis = half, limbs = a third. Heavy rounds hit harder.
local function damageForHit(bone, causer)
    bone = (bone or ""):lower()
    local frac = 0.34
    if bone:find("head") or bone:find("neck") or bone:find("facial") or bone:find("jaw") then
        frac = 1.0
    elseif bone:find("spine") or bone:find("pelvis") or bone:find("clavicle") or bone:find("rib") then
        frac = 0.5
    end
    causer = (causer or ""):lower()
    if causer:find("melee", 1, true) then
        return (frac >= 1.0) and 0.34 or 0.15  -- punches / bats / knives
    end
    if causer:find("50bmg") or causer:find("12ga") or causer:find("308") or causer:find("44mag") or causer:find("50ae") then
        frac = math.min(1.0, frac * 1.6)
    end
    return frac
end

-- Same call the game's projectile (BP_BT_Projectile_Base) makes when a bullet hits an NPC:
-- EventReceiveDamageNPC(HitResult, <row of BT_DamageCauser_DataTable>, CauserID, ExitWound, PhysHitReact).
local DAMAGE_TABLE = "/Game/BloodTrailWeapons/Data/DataTables/BT_DamageCauser_DataTable.BT_DamageCauser_DataTable"
-- Looked up fresh every time: row pointers go stale when the game reloads the level.
local function causerRow(id)
    local dt = StaticFindObject(DAMAGE_TABLE)
    if not valid(dt) then
        pcall(LoadAsset, DAMAGE_TABLE:match("^(.-)%."))
        dt = StaticFindObject(DAMAGE_TABLE)
    end
    if not valid(dt) then return nil end
    local found = nil
    dt:ForEachRow(function(name, row)
        if not found and tostring(name) == id then found = row end
    end)
    return found
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

local applyingRemoteHit = false  -- set while replaying the other player's hit, so it is not sent back
local onNpcHit = nil             -- NPC sync handler (defined further down)

local function installHitHook()
    if hitHookInstalled then return end
    hitHookInstalled = true
    local ok, err = pcall(RegisterHook, NPC_CLASS .. ":EventReceiveDamageNPC",
        function(ctx, HitResult, DamageCauserStruct, DamageCauserID, ExitWound)
            if applyingRemoteHit then return end
            local npc = ctx:get()
            local bone, causer, hx, hy, hz, nx, ny, nz = "", "", nil, nil, nil, 0, 0, 1
            pcall(function()
                local hr = p(HitResult)
                bone = hr.BoneName:ToString()
                hx, hy, hz = hr.ImpactPoint.X, hr.ImpactPoint.Y, hr.ImpactPoint.Z
                nx, ny, nz = hr.ImpactNormal.X, hr.ImpactNormal.Y, hr.ImpactNormal.Z
            end)
            pcall(function() causer = p(DamageCauserID):ToString() end)
            local exitWound = p(ExitWound) and true or false
            if not isPuppet(npc) then
                if onNpcHit and hx then onNpcHit(npc, bone, hx, hy, hz, nx, ny, nz, causer, exitWound) end
                return
            end
            if puppetDead then return end
            if exitWound then return end  -- the entry wound already counted
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
            log(string.format("hit the other player: %s with %s -> %.0f%% of their health", bone, causer, dmg * 100))
            sendGame(string.format("hit %.2f %s %s", dmg, bone ~= "" and bone or "-", causer ~= "" and causer or "-"))
        end)
    log((ok and "hooked " or "could not hook ") .. "EventReceiveDamageNPC" .. (ok and "" or (": " .. tostring(err))))
end

local pup = nil   -- cached components of the remote body (property lookups are slow; do them once)

local function destroyPuppet()
    if valid(puppet) then pcall(function() puppet:K2_DestroyActor() end) end
    puppet, puppetLast, pup = nil, nil, nil
end

local function spawnPuppet(x, y, z, yaw)
    pcall(LoadAsset, NPC_PKG)
    local cls = StaticFindObject(NPC_CLASS)
    local world = getWorld()
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
    -- Stay in "walking" mode (no fall animation) but don't let the movement component simulate.
    pcall(function() a.CharacterMovement:SetMovementMode(1, 0) end)
    pcall(function() a.CharacterMovement:SetComponentTickEnabled(false) end)
    pcall(function() a.CharacterMovement.GravityScale = 0 end)
    pcall(function() a.CapsuleComponent:SetCollisionEnabled(0) end)  -- don't block players; the mesh still takes bullets
    pcall(function() a.HealthOverall = 100000 end)                   -- never dies locally; the owner decides
    pcall(function() a.DebugTextNPC:SetHiddenInGame(true, false) end)
    pcall(function() a.DebugTextLocal:SetHiddenInGame(true, false) end)
    pcall(function() a.StaticMesh:SetVisibility(false, true) end)    -- the NPC's own pistol: never shown
    pcall(function() a.Slidemesh:SetVisibility(false, true) end)
    installHitHook()
    -- The game's locomotion animation (ALS) reads where a character LOOKS from its controller. A
    -- body without one "looks" at yaw 0 forever, so the animation kept playing turn-on-the-spot
    -- steps against the yaw we set: crossed, spinning legs (v0.3 test). Each body gets a bare
    -- AIController - no behaviour tree, no perception - that only carries the look rotation.
    local okC, ctrl = pcall(function()
        local c = spawnActor("/Script/AIModule.AIController")
        if not c then return nil end
        c:Possess(a)
        c:SetControlRotation({ Pitch = 0, Yaw = yaw, Roll = 0 })
        return c
    end)
    return a, (okC and valid(ctrl)) and ctrl or nil
end

-- The movement component is not ticking (we place the bodies ourselves), so its "last update"
-- rotation/location never changed. ALS foot locking measures how far the body turned since then
-- and twisted the planted feet by that (growing) angle every frame: legs spinning like a propeller.
-- Keep them current.
local function syncMovementBase(actor, cm)
    pcall(function()
        local r = actor:K2_GetActorRotation()
        local qx, qy, qz, qw = rotToQuat(r.Pitch, r.Yaw, r.Roll)
        cm.LastUpdateRotation = { X = qx, Y = qy, Z = qz, W = qw }
        local l = actor:K2_GetActorLocation()
        cm.LastUpdateLocation = { X = l.X, Y = l.Y, Z = l.Z }
    end)
end

-- Visual body with full upper-body control: a PoseableMesh copies the NPC's animated pose (legs,
-- walking, crouch) every frame; we then override head and arms. The animated NPC mesh stays as the
-- invisible hit/gore body (collision + wound painting); its gore materials are shared with the copy.
local BODY_PARTS = { "Costume", "hair", "Eyes", "Mouth" }
local function setupBody(a)
    local P = { actor = a, mesh = a.Mesh, cap = a.CapsuleComponent, cm = a.CharacterMovement }
    P.anim = P.mesh:GetAnimInstance()
    P.relLoc = P.mesh.RelativeLocation
    P.relLoc = { X = P.relLoc.X, Y = P.relLoc.Y, Z = P.relLoc.Z }
    local rr = P.mesh.RelativeRotation
    P.relRot = { Pitch = rr.Pitch, Yaw = rr.Yaw, Roll = rr.Roll }
    pcall(function() P.anim.OverlayState = 0 end)                     -- relaxed, empty-hand stance
    -- Don't let VR hands grab or drag the other player (VRTraceChannel = GameTraceChannel1 = 14).
    pcall(function() P.mesh:SetCollisionResponseToChannel(14, 0) end)
    local ok, err = pcall(function()
        local cls = StaticFindObject("/Script/Engine.PoseableMeshComponent")
        local pose = a:AddComponentByClass(cls, true, { Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Translation = { X = 0, Y = 0, Z = 0 }, Scale3D = { X = 1, Y = 1, Z = 1 } }, false)
        if not valid(pose) then error("AddComponentByClass returned nothing") end
        pose:K2_AttachToComponent(P.mesh, FName("None"), 2, 2, 2, true)     -- snap onto the NPC mesh
        pose:SetSkeletalMesh(P.mesh.SkeletalMesh, true)
        P.mesh.VisibilityBasedAnimTickOption = 0                            -- keep animating while hidden
        P.pose = pose
        -- every other skinned part of the NPC (clothes, hair, eyes, mouth, merged costume...) follows the IK copy
        P.parts = {}
        local skinCls = StaticFindObject("/Script/Engine.SkinnedMeshComponent")
        local poseName, meshName = pose:GetFullName(), P.mesh:GetFullName()
        local okC, comps = pcall(function() return a:K2_GetComponentsByClass(skinCls) end)
        if not okC or not comps then okC, comps = pcall(function() return a:GetComponentsByClass(skinCls) end) end
        local function consider(c)
            if valid(c) then
                local n = c:GetFullName()
                if n ~= poseName and n ~= meshName then P.parts[#P.parts + 1] = c end
            end
        end
        if okC and comps then
            if type(comps) == "table" then
                for _, c in ipairs(comps) do consider(c) end
            else
                pcall(function() comps:ForEach(function(_, e) consider(e:get()) end) end)
            end
        end
        if #P.parts == 0 then
            for _, n in ipairs(BODY_PARTS) do pcall(function() if valid(a[n]) then P.parts[#P.parts + 1] = a[n] end end) end
        end
        for _, c in ipairs(P.parts) do pcall(function() c:SetMasterPoseComponent(pose, true) end) end
        P.hideMesh = function()
            P.mesh:SetVisibility(false, false)                              -- children (wounds, decals) stay
            pcall(function() P.mesh:SetRenderInMainPass(false) end)
            for _, c in ipairs(P.parts) do pcall(function() c:SetMasterPoseComponent(pose, true) end) end
        end
        P.hideMesh()
    end)
    if not ok then
        log("full-body IK unavailable (" .. tostring(err) .. "), using the animated body only")
        P.pose = nil
        pcall(function() P.mesh:SetVisibility(true, false) end)
    end
    return P
end

local function copyBodyMaterials(P)
    if not P.pose then return end
    pcall(function()
        local n = P.mesh:GetNumMaterials()
        for i = 0, n - 1 do P.pose:SetMaterial(i, P.mesh:GetMaterial(i)) end
    end)
end

local function boneT(pose, name)
    local t = pose:GetBoneTransformByName(FName(name), 0)            -- 0 = world space
    return { t.Translation.X, t.Translation.Y, t.Translation.Z }, { t.Rotation.X, t.Rotation.Y, t.Rotation.Z, t.Rotation.W }
end
local function setBoneRot(pose, name, pos, q)
    pose:SetBoneTransformByName(FName(name), { Rotation = { X = q[1], Y = q[2], Z = q[3], W = q[4] },
        Translation = { X = pos[1], Y = pos[2], Z = pos[3] }, Scale3D = { X = 1, Y = 1, Z = 1 } }, 0)
end
-- rotate bone `name` so that the direction to `child` points along (dx,dy,dz)
local function aimBone(pose, name, child, dx, dy, dz)
    local bp, bq = boneT(pose, name)
    local cp = boneT(pose, child)
    local d = fromTo(cp[1] - bp[1], cp[2] - bp[2], cp[3] - bp[3], dx, dy, dz)
    setBoneRot(pose, name, bp, qnorm(qmul(d, bq)))
    return bp
end

-- Two-bone arm IK: shoulder -> elbow -> wrist reaches the controller; elbow bends down/out/back.
local function solveArm(P, side, cx, cy, cz, cq, yaw)
    local pose = P.pose
    local up, lo, hand, fing = "upperarm_" .. side, "lowerarm_" .. side, "hand_" .. side, "middle_01_" .. side
    local S = boneT(pose, up)
    local Ep = boneT(pose, lo)
    local Hp = boneT(pose, hand)
    local a = math.sqrt(dist2(S[1], S[2], S[3], Ep[1], Ep[2], Ep[3]))
    local b = math.sqrt(dist2(Ep[1], Ep[2], Ep[3], Hp[1], Hp[2], Hp[3]))
    -- wrist sits ~7 cm behind the controller's grip point
    local fx, fy, fz = qv(cq, 1, 0, 0)
    local T = { cx - fx * 7, cy - fy * 7, cz - fz * 7 }
    local dx, dy, dz = T[1] - S[1], T[2] - S[2], T[3] - S[3]
    local d = math.sqrt(dx * dx + dy * dy + dz * dz)
    if d < 1 then return end
    local dmax = (a + b) * 0.999
    if d > dmax then d = dmax end
    dx, dy, dz = dx / math.sqrt(dx * dx + dy * dy + dz * dz), dy / math.sqrt(dx * dx + dy * dy + dz * dz), dz / math.sqrt(dx * dx + dy * dy + dz * dz)
    local x = (a * a - b * b + d * d) / (2 * d)
    local h = math.sqrt(math.max(0, a * a - x * x))
    -- pole: down, outward and slightly back relative to the body
    local s = side == "l" and -1 or 1
    local byaw = yaw * DEG
    local Fx, Fy = math.cos(byaw), math.sin(byaw)
    local Rx, Ry = -Fy, Fx
    local px, py, pz = Rx * s * 0.6 - Fx * 0.3, Ry * s * 0.6 - Fy * 0.3, -1.0
    local pd = px * dx + py * dy + pz * dz
    px, py, pz = px - dx * pd, py - dy * pd, pz - dz * pd
    local pl = math.sqrt(px * px + py * py + pz * pz)
    if pl < 1e-4 then px, py, pz, pl = 0, 0, -1, 1 end
    px, py, pz = px / pl, py / pl, pz / pl
    local E = { S[1] + dx * x + px * h, S[2] + dy * x + py * h, S[3] + dz * x + pz * h }
    aimBone(pose, up, lo, E[1] - S[1], E[2] - S[2], E[3] - S[3])
    local E2 = boneT(pose, lo)
    aimBone(pose, lo, hand, T[1] - E2[1], T[2] - E2[2], T[3] - E2[3])
    -- hand: fingers point along the controller's forward
    pcall(aimBone, pose, hand, fing, fx, fy, fz)
end

-- Head (and neck) follow the headset exactly: look up, down, left, right, tilt.
local function solveHead(P, hq, actorYaw)
    local pose = P.pose
    local hp, cur = boneT(pose, "Head")
    if not P.headOff then
        -- the head's rotation relative to "looking straight ahead along the body", taken once
        local bq = { rotToQuat(0, actorYaw, 0) }
        P.headOff = qmul(qconj(bq), cur)
    end
    local target = qnorm(qmul(hq, P.headOff))
    -- the neck takes half of the turn so a 90-degree look doesn't snap the head off the shoulders
    pcall(function()
        local d = qmul(target, qconj(cur))
        if d[4] < 0 then d = { -d[1], -d[2], -d[3], -d[4] } end
        local half = qnorm({ d[1], d[2], d[3], d[4] + 1 })
        local np, nq = boneT(pose, "neck_01")
        setBoneRot(pose, "neck_01", np, qnorm(qmul(half, nq)))
        hp = boneT(pose, "Head")
    end)
    setBoneRot(pose, "Head", hp, target)
end

-- Things the other player holds (gun, magazine, knife...), drawn at the exact streamed transform.
-- remoteHeld[side] = { ver, n, ms, parts = { [i] = { path, kind, rel pos/rot, scale } } }
local remoteHeld = { L = { ver = 0, n = 0, parts = {} }, R = { ver = 0, n = 0, parts = {} } }
local heldShown = { L = nil, R = nil }
local function dropShown(h)
    if not h then return end
    for _, a in ipairs(h.actors) do if valid(a) then pcall(function() a:K2_DestroyActor() end) end end
end
local function destroyHeld()
    for side, h in pairs(heldShown) do dropShown(h) heldShown[side] = nil end
end

local function meshPartActor(part)
    local asset = findAsset(part.path)
    if not asset then return nil end
    local a
    if part.kind == "K" then
        a = spawnActor("/Script/Engine.SkeletalMeshActor")
        if a then local c = a.SkeletalMeshComponent c:SetMobility(2) c:SetSkeletalMesh(asset, true) c:SetCollisionEnabled(0) end
    else
        a = spawnActor("/Script/Engine.StaticMeshActor")
        if a then local c = a.StaticMeshComponent c:SetMobility(2) c:SetStaticMesh(asset) c:SetCollisionEnabled(0) end
    end
    return a
end

local function updateHeldItem(side, on, x, y, z, qx, qy, qz, qw)
    local want = remoteHeld[side]
    local h = heldShown[side]
    if h and (h.ver ~= want.ver or not valid(h.actors[1])) then dropShown(h) h, heldShown[side] = nil, nil end
    local complete = want.n > 0
    for i = 1, want.n do if not want.parts[i] then complete = false end end
    if not on or not complete then
        if h and not h.hidden then
            for _, a in ipairs(h.actors) do pcall(function() a:SetActorHiddenInGame(true) end) end
            h.hidden = true
        end
        return
    end
    if not h then
        if want.failedVer == want.ver then return end   -- nothing of this item could be shown; don't retry every frame
        h ={ ver = want.ver, actors = {}, parts = {} }
        local names = {}
        for i = 1, want.n do
            local part = want.parts[i]
            local okM, a = pcall(meshPartActor, part)
            if not okM then log("held item part failed: " .. tostring(a)) a = nil end
            if a then
                h.actors[#h.actors + 1] = a
                h.parts[#h.parts + 1] = part
                names[#names + 1] = part.path:match("([^/%.]+)$") or part.path
            elseif want.missingLogged ~= part.path then
                want.missingLogged = part.path
                log("held item mesh not found: " .. part.path)
            end
        end
        if #h.actors == 0 then want.failedVer = want.ver return end
        heldShown[side] = h
        log("other player holds in " .. side .. " hand: " .. table.concat(names, ", "))
    end
    if h.hidden then
        for _, a in ipairs(h.actors) do pcall(function() a:SetActorHiddenInGame(false) end) end
        h.hidden = false
    end
    local mq, ms = { qx, qy, qz, qw }, want.ms or 1
    for i, a in ipairs(h.actors) do
        local part = h.parts[i]
        local ox, oy, oz = qv(mq, part.x * ms, part.y * ms, part.z * ms)
        local q = qmul(mq, part.q)
        local p, yw, r = quatToRot(q[1], q[2], q[3], q[4])
        a:K2_SetActorLocationAndRotation({ X = x + ox, Y = y + oy, Z = z + oz }, { Pitch = p, Yaw = yw, Roll = r }, false, {}, true)
        if part.appliedScale ~= part.s * ms then
            part.appliedScale = part.s * ms
            a:SetActorScale3D({ X = part.appliedScale, Y = part.appliedScale, Z = part.appliedScale })
        end
    end
end

local function half_default()
    local h = 90
    pcall(function() h = pup.cap:GetScaledCapsuleHalfHeight() end)
    return h
end

local function worldTime()
    local ok, t = pcall(function() return UEHelpers.GetGameplayStatics():GetTimeSeconds(getWorld()) end)
    return ok and t or nil
end

-- v: head(1-7) left(8-14) right(15-21) body(22-28) heldL(29-35) heldR(36-42) flags(43)
local function updatePuppet(v)
    if puppetFailed then return false end
    if not valid(puppet) then
        if puppet ~= nil then log("remote player body disappeared (destroyed by the game?) - respawning") puppet, pup = nil, nil end
        if puppetDead then return true end
        local headYaw = select(2, quatToRot(v[4], v[5], v[6], v[7]))
        local ok, a, ctrl = pcall(spawnPuppet, v[22], v[23], v[24] + 90, headYaw)
        if not ok or not a then
            log("could not spawn the remote player body (" .. tostring(a) .. "), using simple shapes")
            puppetFailed = true
            return false
        end
        puppet = a
        local okB, P = pcall(setupBody, a)
        if not okB then log("body setup failed: " .. tostring(P)) destroyPuppet() puppetFailed = true return false end
        pup = P
        pup.bodyYaw = headYaw
        pup.ctrl = ctrl
        if ctrl then
            -- "looking direction" mode: the body faces where the player looks and walking sideways or
            -- backwards plays strafe / backpedal steps instead of turning to run the other way
            pcall(function() a:BPI_Set_RotationMode(1) end)
            pcall(function() a.DesiredRotationMode = 1 a.RotationMode = 1 end)
        end
        pcall(function() pup.maxAccel = pup.cm.MaxAcceleration end)
        copyBodyMaterials(pup)
        log("remote player body spawned: " .. a:GetFullName() .. (pup.pose and " (full-body IK)" or "") .. (ctrl and " (look control)" or " (no look control)"))
    end
    if puppetDead then return true end
    local P = pup
    local flags = math.floor(v[43] or 0)

    -- Never fall over / never be dragged while the owner is alive: any ragdoll (hit reaction,
    -- someone grabbing the body) is undone the same frame.
    local okS, sim = pcall(function() return P.mesh:IsSimulatingPhysics(FName("Pelvis")) end)
    if okS and sim then
        pcall(function()
            P.mesh:SetSimulatePhysics(false)
            P.mesh:K2_AttachToComponent(P.cap, FName("None"), 2, 2, 2, true)
            P.mesh:K2_SetRelativeLocationAndRotation(P.relLoc, P.relRot, false, {}, true)
        end)
    end

    local headPitch, headYaw = quatToRot(v[4], v[5], v[6], v[7])
    local now = worldTime() or 0
    local dt = P.lastT and math.max(1e-3, math.min(0.2, now - P.lastT)) or 1 / 90
    P.lastT = now
    local speed = math.sqrt((P.velX or 0) ^ 2 + (P.velY or 0) ^ 2)
    local setRot = false
    if P.ctrl and valid(P.ctrl) then
        -- The game's own animation turns the body: when standing it steps round after the head
        -- (turn in place), when moving it faces the look direction and strafes / backpedals.
        pcall(function() P.ctrl:SetControlRotation({ Pitch = headPitch, Yaw = headYaw, Roll = 0 }) end)
        local ry = puppet:K2_GetActorRotation().Yaw
        -- safety net: never leave the body facing far away from the head for long
        local off = normAxis(headYaw - ry)
        P.farFrames = math.abs(off) > 110 and (P.farFrames or 0) + 1 or 0
        if P.farFrames > 60 then ry = normAxis(ry + off * math.min(1, dt * 5)) setRot = true end
        P.bodyYaw = ry
    else
        -- fallback without a controller: body yaw follows the head with a 50-degree dead zone
        local diff = normAxis(headYaw - P.bodyYaw)
        if math.abs(diff) > 50 then P.bodyYaw = normAxis(headYaw - (diff > 0 and 50 or -50)) end
        if speed > 40 then P.bodyYaw = normAxis(P.bodyYaw + diff * math.min(1, dt * 4)) end
        setRot = true
    end
    local by = P.bodyYaw * DEG
    local half = P.half or half_default()
    P.half = half
    local x, y, z = v[1] - math.cos(by) * 10, v[2] - math.sin(by) * 10, v[24] + half
    if setRot then
        puppet:K2_SetActorLocationAndRotation({ X = x, Y = y, Z = z }, { Pitch = 0, Yaw = P.bodyYaw, Roll = 0 }, false, {}, true)
    else
        puppet:K2_SetActorLocation({ X = x, Y = y, Z = z }, false, {}, true)
    end

    -- Walk animation speed from the real movement over real time (v0.2 assumed 90 fps -> it
    -- looked like sprinting). Small movements (leaning, VR room-scale shuffles) don't walk.
    -- The body's own position shifts when it turns, so speed comes from the streamed position.
    if P.lastX then
        local vx, vy = (v[22] - P.lastX) / dt, (v[23] - P.lastY) / dt
        P.velX = (P.velX or 0) * 0.85 + vx * 0.15
        P.velY = (P.velY or 0) * 0.85 + vy * 0.15
        local sp = math.sqrt(P.velX ^ 2 + P.velY ^ 2)
        local wx, wy = P.velX, P.velY
        if sp < 25 then wx, wy, sp = 0, 0, 0 end
        pcall(function() P.cm.Velocity = { X = wx, Y = wy, Z = 0 } end)
        -- "has movement input": without it the animation slides in the idle pose below 1.5 m/s
        pcall(function()
            local acc = P.maxAccel or 1500
            if sp > 0 then P.cm.Acceleration = { X = wx / sp * acc, Y = wy / sp * acc, Z = 0 }
            else P.cm.Acceleration = { X = 0, Y = 0, Z = 0 } end
        end)
    end
    P.lastX, P.lastY = v[22], v[23]
    syncMovementBase(puppet, P.cm)
    local crouch = (v[3] - v[24] < 120) and 1 or 0                         -- crouch when the real head is low
    if crouch ~= P.crouch then
        P.crouch = crouch
        pcall(function() puppet.DesiredStance = crouch puppet.Stance = crouch end)
        pcall(function() P.anim.Stance = crouch end)
    end

    -- Upper body: exact head direction + arms reaching each controller.
    if P.pose then
        local okI, errI = pcall(function()
            P.pose:CopyPoseFromSkeletalComponent(P.mesh)
            solveHead(P, { v[4], v[5], v[6], v[7] }, P.bodyYaw)
            if (flags & 1) ~= 0 then solveArm(P, "l", v[8], v[9], v[10], { v[11], v[12], v[13], v[14] }, P.bodyYaw) end
            if (flags & 2) ~= 0 then solveArm(P, "r", v[15], v[16], v[17], { v[18], v[19], v[20], v[21] }, P.bodyYaw) end
        end)
        if not okI then
            P.ikErrors = (P.ikErrors or 0) + 1
            if P.ikErrors == 1 or P.ikErrors % 600 == 0 then log("body IK error: " .. tostring(errI)) end
        end
        if frameNo % 30 == 0 then copyBodyMaterials(P) end
        if frameNo % 60 == 0 and P.hideMesh then pcall(P.hideMesh) end   -- the NPC's own logic may re-show it
        if selfTest ~= "" and frameNo % 450 == 0 then
            pcall(function()
                local names = {}
                for _, c in ipairs(P.parts) do names[#names + 1] = c:GetFName():ToString() .. (c:IsVisible() and "" or "(hidden)") end
                log("body parts following IK: " .. table.concat(names, ", ") .. " | NPC mesh visible: " .. tostring(P.mesh:IsVisible()))
            end)
        end
    else
        -- fallback (no poseable mesh): the rig's own look-at
        pcall(function()
            local fx2, fy2, fz2 = qrot(v[4], v[5], v[6], v[7], 300, 0, 0)
            P.anim["Look At Location"] = { Translation = { X = v[1] + fx2, Y = v[2] + fy2, Z = v[3] + fz2 },
                                           Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Scale3D = { X = 1, Y = 1, Z = 1 } }
            P.anim.DragonLookAlpha = 1.0
        end)
    end

    -- held items (gun / magazine / melee) exactly where they are in the other player's hands
    local okHL, eHL = pcall(updateHeldItem, "L", (flags & 4) ~= 0, v[29], v[30], v[31], v[32], v[33], v[34], v[35])
    local okHR, eHR = pcall(updateHeldItem, "R", (flags & 8) ~= 0, v[36], v[37], v[38], v[39], v[40], v[41], v[42])
    if (not okHL or not okHR) and frameNo % 300 == 0 then log("held item display error: " .. tostring(eHL) .. " / " .. tostring(eHR)) end
    if selfTest ~= "" and frameNo % 450 == 0 then
        log(string.format("held debug: flags %d, remote R parts %d/%d", flags, #remoteHeld.R.parts, remoteHeld.R.n))
    end
    local okL, eL = pcall(EX.updateRemoteLights, v)
    if not okL and frameNo % 300 == 0 then log("remote lights error: " .. tostring(eL)) end

    if selfTest ~= "" and frameNo % 450 == 0 and P.pose then
        pcall(function()
            local hb = boneT(P.pose, "Head")
            local lh = boneT(P.pose, "hand_l")
            local rh = boneT(P.pose, "hand_r")
            if heldShown.R and valid(heldShown.R.actors[1]) then
                local g = heldShown.R.actors[1]:K2_GetActorLocation()
                log(string.format("held item R: %.0f cm from the right hand bone, visible %s", math.sqrt(dist2(g.X, g.Y, g.Z, rh[1], rh[2], rh[3])), tostring(not heldShown.R.hidden)))
            end
            log(string.format("body check: head bone Z %.0f vs headset %.0f | left hand %.0f cm from controller, right hand %.0f cm | body yaw %.0f head yaw %.0f | speed %.0f",
                hb[3], v[3], math.sqrt(dist2(lh[1], lh[2], lh[3], v[8], v[9], v[10])), math.sqrt(dist2(rh[1], rh[2], rh[3], v[15], v[16], v[17])),
                P.bodyYaw, headYaw, speed))
        end)
    end
    return true
end

local function puppetDie()
    if puppetDead then return end
    puppetDead = true
    destroyHeld()
    pcall(EX.hideRemoteLights)
    if valid(puppet) then
        log("other player died: ragdolling their body")
        -- show the real (gore-painted) NPC mesh for the ragdoll
        pcall(function()
            if pup and pup.pose then
                pup.pose:SetVisibility(false, false)
                for _, n in ipairs(BODY_PARTS) do pcall(function() puppet[n]:SetMasterPoseComponent(pup.mesh, true) end) end
                pup.mesh:SetVisibility(true, false)
            end
        end)
        local ok = pcall(function() puppet:RagdollStart() end)
        if not ok then pcall(function() puppet.Mesh:SetSimulatePhysics(true) end) end
    end
end

local function puppetRespawn()
    puppetDead = false
    destroyPuppet()
end

------------------------------------------------------------------------------- NPC sync (host-authoritative)
-- Host: its game runs the real NPCs (AI, spawning, health). ~10x/s it sends each NPC's position,
--       yaw, speed and dead flag. Hits the host lands are replayed on the client's copies.
-- Client: removes the NPCs its own game spawns and shows AI-less mirrors of the host's NPCs.
--       Hits the client lands on a mirror show gore immediately and are replayed on the host's real
--       NPC (which may die there; the mirror then ragdolls). Same gore pipeline on both sides.
local NPC_SYNC_FRAMES = 9
npcSyncEnabled = true  -- global on purpose: set from the CFG reply
local npcIdOf = {}        -- host: actor full name -> id
local npcActor = {}       -- host: id -> real NPC;  client: id -> mirror
local mirrorIdOf = {}     -- client: mirror full name -> id
local mirrorTarget = {}   -- client: id -> { x, y, z, yaw, vx, vy, dead, seen }
local nextNpcId = 1
local syncMap = nil

local function syncActive()
    return status.state == "Connected" and currentMap ~= nil and status.sameMap
end

local function resetNpcSync()
    for _, a in pairs(npcActor) do
        if status.role == "client" and valid(a) then pcall(function() a:K2_DestroyActor() end) end
    end
    npcIdOf, npcActor, mirrorIdOf, mirrorTarget, nextNpcId = {}, {}, {}, {}, 1
end

-- NPCs are tracked as they are created (NotifyOnNewObject) instead of scanning every object in
-- the game several times a second (FindAllOf), which was expensive.
local npcRegistry = {}
local npcRegistryHooked = false
local lastRegistryScan = -100000
local function ensureNpcRegistry()
    if npcRegistryHooked then return end
    npcRegistryHooked = true
    local ok, err = pcall(NotifyOnNewObject, NPC_CLASS, function(obj)
        npcRegistry[#npcRegistry + 1] = obj
    end)
    if not ok then log("NPC tracking hook failed (" .. tostring(err) .. "), using slow scans") end
end
local function liveNpcs()
    -- one fallback scan per map (NPCs that existed before the hook), then the registry only
    if frameNo - lastRegistryScan > 100000 or (#npcRegistry == 0 and frameNo - lastRegistryScan > 900) then
        lastRegistryScan = frameNo
        for _, a in ipairs(FindAllOf("BP_NPC_Man_C") or {}) do npcRegistry[#npcRegistry + 1] = a end
    end
    local out, seen, keep = {}, {}, {}
    for _, a in ipairs(npcRegistry) do
        if valid(a) then
            local n = a:GetFullName()
            if not seen[n] and not n:find("Default__", 1, true) then
                seen[n] = true
                keep[#keep + 1] = a
                out[#out + 1] = { actor = a, name = n }
            end
        end
    end
    npcRegistry = keep
    return out
end

local lastBodySent = {}   -- host: id -> {x,y,z} of the last corpse position sent

local function hostSyncTick()
    local alive = {}
    for _, e in ipairs(liveNpcs()) do
        local a, name = e.actor, e.name
        if not isPuppet(a) then
            local id = npcIdOf[name]
            if not id then
                id = nextNpcId
                nextNpcId = nextNpcId + 1
                npcIdOf[name] = id
                npcActor[id] = a
                installHitHook()
                EX.installFireHook()
            end
            alive[id] = true
            local dead = false
            pcall(function() dead = a.Dead == true end)
            if not dead then
                local l = a:K2_GetActorLocation()
                local r = a:K2_GetActorRotation()
                local vx, vy = 0, 0
                pcall(function() local v = a:GetVelocity() vx, vy = v.X, v.Y end)
                sendGame(string.format("npc %d %.1f %.1f %.1f %.1f %.0f %.0f 0", id, l.X, l.Y, l.Z, r.Yaw, vx, vy))
            else
                -- corpse: stream where the body (pelvis) is while it moves (falling, being dragged)
                local okP, pl = pcall(function() return a.Mesh:GetSocketLocation(FName("Pelvis")) end)
                if okP then
                    local last = lastBodySent[id]
                    if not last then sendGame(string.format("npc %d %.1f %.1f %.1f 0 0 0 1", id, pl.X, pl.Y, pl.Z)) end
                    if not last or dist2(last[1], last[2], last[3], pl.X, pl.Y, pl.Z) > 16 then
                        lastBodySent[id] = { pl.X, pl.Y, pl.Z }
                        sendGame(string.format("npcbody %d %.1f %.1f %.1f", id, pl.X, pl.Y, pl.Z))
                    end
                end
            end
        end
    end
    for id, _ in pairs(npcActor) do
        if not alive[id] then
            sendGame("npcgone " .. id)
            npcActor[id] = nil
            lastBodySent[id] = nil
        end
    end
end

local function clientSyncTick()
    -- remove NPCs that this game spawned by itself (the host's game decides which NPCs exist)
    if frameNo % NPC_SYNC_FRAMES == 0 then
        for _, e in ipairs(liveNpcs()) do
            if not isPuppet(e.actor) and not mirrorIdOf[e.name] then
                pcall(function() e.actor:K2_DestroyActor() end)
            end
        end
    end
    for id, t in pairs(mirrorTarget) do
        local m = npcActor[id]
        if not valid(m) then
            local ok, a, ctrl = pcall(spawnPuppet, t.x, t.y, t.z, t.yaw)
            if ok and a then
                m = a
                npcActor[id] = a
                mirrorIdOf[a:GetFullName()] = id
                t.mesh = a.Mesh
                t.ctrl = ctrl
                t.cm = a.CharacterMovement
                pcall(function() a.CapsuleComponent:SetCollisionEnabled(1) end)  -- NPCs block you like normal NPCs
            end
        end
        if valid(m) then
            local mesh = t.mesh
            if t.dead then
                if not t.deadApplied then
                    t.deadApplied = true
                    pcall(function() m:RagdollStart() end)
                end
                -- corpse follows the host's corpse (pulled by the pelvis, limbs stay physical),
                -- except while this player drags or has just thrown it
                if t.bx and not EX.locallyControlled(id) and not EX.remoteGrab[id] then
                    pcall(function()
                        local pl = mesh:GetSocketLocation(FName("Pelvis"))
                        local ex, ey, ez = t.bx - pl.X, t.by - pl.Y, t.bz - pl.Z
                        if ex * ex + ey * ey + ez * ez > 9 then
                            mesh:SetPhysicsLinearVelocity({ X = ex * 8, Y = ey * 8, Z = ez * 8 }, false, FName("Pelvis"))
                        end
                    end)
                end
            else
                local okS, sim = pcall(function() return mesh:IsSimulatingPhysics(FName("Pelvis")) end)
                if okS and sim then
                    -- knocked down on this side but alive on the host: get up again (not while
                    -- someone is holding it)
                    if EX.locallyControlled(id) or EX.remoteGrab[id] then
                        t.downSince = nil
                    else
                        t.downSince = t.downSince or frameNo
                        if frameNo - t.downSince == 30 then pcall(function() m:RagdollEnd() end) end
                    end
                else
                    t.downSince = nil
                    local l = m:K2_GetActorLocation()
                    local k = 0.25
                    local x, y, z = l.X + (t.x - l.X) * k, l.Y + (t.y - l.Y) * k, l.Z + (t.z - l.Z) * k
                    if dist2(l.X, l.Y, l.Z, t.x, t.y, t.z) > 300 * 300 then x, y, z = t.x, t.y, t.z end
                    local yaw = t.yaw
                    if t.aimUntil and frameNo < t.aimUntil then   -- shooting at me
                        yaw = math.atan(localHead[2] - y, localHead[1] - x) * RAD
                    end
                    m:K2_SetActorLocationAndRotation({ X = x, Y = y, Z = z }, { Pitch = 0, Yaw = yaw, Roll = 0 }, false, {}, true)
                    pcall(function() m.CharacterMovement.Velocity = { X = t.vx, Y = t.vy, Z = 0 } end)
                    if t.ctrl and valid(t.ctrl) then pcall(function() t.ctrl:SetControlRotation({ Pitch = 0, Yaw = yaw, Roll = 0 }) end) end
                    if t.cm then syncMovementBase(m, t.cm) end
                end
            end
        end
    end
end

local function npcSyncTick()
    if not syncActive() then
        if syncMap then resetNpcSync() syncMap = nil end
        return
    end
    ensureNpcRegistry()
    if syncMap ~= currentMap then resetNpcSync() lastBodySent = {} syncMap = currentMap lastRegistryScan = -100000 end
    if status.role == "host" then
        if frameNo % NPC_SYNC_FRAMES == 0 then hostSyncTick() end
    else
        clientSyncTick()
    end
end

-- Hits are sent RELATIVE TO THE BONE that was hit, so the wound/blood appears on the same spot of
-- the body on both screens even when the two copies of an NPC are not exactly in the same place
-- (in v0.2 world coordinates were used: blood appeared in mid-air).
local function boneFrame(mesh, bone)
    local bl = mesh:GetSocketLocation(FName(bone))
    local br = mesh:GetSocketRotation(FName(bone))
    return bl, { rotToQuat(br.Pitch, br.Yaw, br.Roll) }
end

onNpcHit = function(npc, bone, x, y, z, nx, ny, nz, causer, exitWound)
    if not syncActive() or causer == "" then return end
    local name = npc:GetFullName()
    local id = status.role == "host" and npcIdOf[name] or mirrorIdOf[name]
    if not id then return end
    if status.role == "client" and causer:find("melee", 1, true) then
        if not (dist2(x, y, z, localLeft[1], localLeft[2], localLeft[3]) < 3600 or dist2(x, y, z, localRight[1], localRight[2], localRight[3]) < 3600) then return end
    end
    local key = "npc" .. id .. "|" .. causer .. "|" .. bone .. "|" .. tostring(exitWound)
    if lastHitFrame[key] and frameNo - lastHitFrame[key] < 20 then return end
    lastHitFrame[key] = frameNo
    if bone == "" or bone == "None" then bone = "Pelvis" end
    local ok, bl, q = pcall(boneFrame, npc.Mesh, bone)
    if not ok then return end
    local iq = qconj(q)
    local ox, oy, oz = qv(iq, x - bl.X, y - bl.Y, z - bl.Z)
    local lx, ly, lz = qv(iq, nx, ny, nz)
    sendGame(string.format("npchit %d %s %.2f %.2f %.2f %.3f %.3f %.3f %s %d", id, bone, ox, oy, oz, lx, ly, lz, causer, exitWound and 1 or 0))
end

local function handleNpcEvent(kind, rest)
    if kind == "npc" and status.role == "client" then
        local id, x, y, z, yaw, vx, vy, dead = rest:match("^(%d+) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+) (%d)")
        id = tonumber(id)
        if not id then return end
        local t = mirrorTarget[id] or {}
        if dead == "1" then
            t.dead = true
            if not t.x then t.x, t.y, t.z, t.yaw, t.vx, t.vy = tonumber(x), tonumber(y), tonumber(z), 0, 0, 0 end
        else
            t.x, t.y, t.z, t.yaw, t.vx, t.vy = tonumber(x), tonumber(y), tonumber(z), tonumber(yaw), tonumber(vx), tonumber(vy)
            t.dead = false
        end
        mirrorTarget[id] = t
    elseif kind == "npcbody" and status.role == "client" then
        local id, x, y, z = rest:match("^(%d+) (%S+) (%S+) (%S+)")
        local t = mirrorTarget[tonumber(id or "")]
        if t then t.bx, t.by, t.bz = tonumber(x), tonumber(y), tonumber(z) end
    elseif kind == "npcgone" and status.role == "client" then
        local id = tonumber(rest)
        if id and valid(npcActor[id]) then pcall(function() npcActor[id]:K2_DestroyActor() end) end
        if id then npcActor[id], mirrorTarget[id] = nil, nil end
    elseif kind == "npchit" then
        local id, bone, ox, oy, oz, lx, ly, lz, causer, ew = rest:match("^(%d+) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+) (%d)")
        local target = npcActor[tonumber(id or "")]
        if not valid(target) then return end
        local ok, bl, q = pcall(boneFrame, target.Mesh, bone)
        if not ok then return end
        local wx, wy, wz = qv(q, tonumber(ox), tonumber(oy), tonumber(oz))
        local nx, ny, nz = qv(q, tonumber(lx), tonumber(ly), tonumber(lz))
        applyingRemoteHit = true
        local okA, err = pcall(applyBulletHit, target, bone, bl.X + wx, bl.Y + wy, bl.Z + wz, nx, ny, nz, causer, ew == "1")
        applyingRemoteHit = false
        if not okA then log("replaying NPC hit failed: " .. tostring(err)) end
    end
end

------------------------------------------------------------------------------- torch / night vision
-- The player pawn (WendigoVrChar) has a HeadLamp spot light, a NightVisionLight and a VisionMode.
-- Their state is sent when it changes; the other game draws a matching head lamp on the body
-- (same light settings, copied from its own player's lamp) and a faint green glow for night vision.
function EX.readVision()
    local pawn = getPawn()
    if not pawn then return end
    local lamp, nv, mode = 0, 0, 0
    pcall(function() mode = tonumber(pawn.VisionMode) or 0 end)
    pcall(function() local l = pawn.HeadLamp if valid(l) and l:IsVisible() and l.Intensity > 0 then lamp = 1 end end)
    pcall(function() local l = pawn.NightVisionLight if valid(l) and l:IsVisible() and l.Intensity > 0 then nv = 1 end end)
    local s = string.format("vision %d %d %d", lamp, nv, mode)
    if s ~= EX.visSent or frameNo - (EX.visAt or 0) > 900 then
        if s ~= EX.visSent then log(string.format("my head lamp %s, night vision %s (vision mode %d)", lamp == 1 and "on" or "off", nv == 1 and "on" or "off", mode)) end
        EX.visSent, EX.visAt = s, frameNo
        sendGame(s)
    end
end

local function lightActor(classPath, prop)
    local a = spawnActor(classPath)
    if not a then return nil, nil end
    local c = a[prop]
    pcall(function() c:SetMobility(2) end)
    return a, c
end

function EX.hideRemoteLights()
    for _, k in ipairs({ "lampActor", "nvActor" }) do
        if valid(EX[k]) then pcall(function() EX[k]:SetActorHiddenInGame(true) end) end
    end
    EX.lampShown, EX.nvShown = false, false
end

-- v: the remote pose (head = v[1..7])
function EX.updateRemoteLights(v)
    local hq = { v[4], v[5], v[6], v[7] }
    local p, yw, r = quatToRot(hq[1], hq[2], hq[3], hq[4])
    local wantLamp = EX.remoteVis.lamp == 1 and not puppetDead
    if wantLamp and not valid(EX.lampActor) then
        local a, c = lightActor("/Script/Engine.SpotLight", "SpotLightComponent")
        if a then
            EX.lampActor = a
            local src = nil
            pcall(function() local pw = getPawn() if pw then src = pw.HeadLamp end end)
            if valid(src) then
                pcall(function() c.IntensityUnits = src.IntensityUnits end)
                pcall(function() c:SetIntensity(src.Intensity) end)
                pcall(function() c:SetOuterConeAngle(src.OuterConeAngle) end)
                pcall(function() c:SetInnerConeAngle(src.InnerConeAngle) end)
                pcall(function() c:SetAttenuationRadius(src.AttenuationRadius) end)
                pcall(function() local lc = src.LightColor c:SetLightColor({ R = lc.R / 255, G = lc.G / 255, B = lc.B / 255, A = 1 }, true) end)
                pcall(function() c:SetCastShadows(src.CastShadows) end)
            else
                pcall(function() c:SetIntensity(5000) c:SetOuterConeAngle(28) c:SetInnerConeAngle(12) c:SetAttenuationRadius(2500) end)
            end
            log("other player's head lamp created")
        end
    end
    if valid(EX.lampActor) then
        if wantLamp ~= EX.lampShown then
            EX.lampShown = wantLamp
            pcall(function() EX.lampActor:SetActorHiddenInGame(not wantLamp) end)
        end
        if wantLamp then
            local ox, oy, oz = qv(hq, 10, 0, 9)          -- on the forehead
            EX.lampActor:K2_SetActorLocationAndRotation({ X = v[1] + ox, Y = v[2] + oy, Z = v[3] + oz }, { Pitch = p, Yaw = yw, Roll = r }, false, {}, true)
        end
    end
    local wantNv = EX.remoteVis.nv == 1 and not puppetDead
    if wantNv and not valid(EX.nvActor) then
        local a, c = lightActor("/Script/Engine.PointLight", "PointLightComponent")
        if a then
            EX.nvActor = a
            pcall(function() c:SetIntensity(30) c:SetAttenuationRadius(18) c:SetCastShadows(false) end)
            pcall(function() c:SetLightColor({ R = 0.25, G = 1.0, B = 0.3, A = 1 }, true) end)
        end
    end
    if valid(EX.nvActor) then
        if wantNv ~= EX.nvShown then
            EX.nvShown = wantNv
            pcall(function() EX.nvActor:SetActorHiddenInGame(not wantNv) end)
        end
        if wantNv then
            local ox, oy, oz = qv(hq, 12, 0, 2)          -- just in front of the eyes
            EX.nvActor:K2_SetActorLocation({ X = v[1] + ox, Y = v[2] + oy, Z = v[3] + oz }, false, {}, true)
        end
    end
end

------------------------------------------------------------------------------- grabbing and throwing
-- NPC bodies: the player who drags a body streams where the grabbed bone is; the other game pulls
-- the same bone of its copy there (physics), and the release velocity is replayed so a throw lands
-- in the same place. While you drag a body, your game ignores the other side's corrections for it.
local function npcSyncIdOf(npc)
    local name = npc:GetFullName()
    if status.role == "host" then return npcIdOf[name] end
    return mirrorIdOf[name]
end

function EX.grabTick()
    local pawn = getPawn()
    if not pawn then return end
    local now = {}
    for _, side in ipairs({ "R", "L" }) do
        local npc = nil
        pcall(function() npc = pawn["GrippedNpc" .. side] end)
        if valid(npc) and not isPuppet(npc) then
            local id = npcSyncIdOf(npc)
            if id then
                local bone = "Pelvis"
                pcall(function() local b = npc.GrippedBoneName:ToString() if b ~= "" and b ~= "None" then bone = b end end)
                now[id] = { npc = npc, bone = bone }
            end
        end
    end
    for id, g in pairs(now) do
        if not EX.grab[id] then log(string.format("grabbed NPC %d by the %s", id, g.bone)) end
        if frameNo % 3 == 0 then
            pcall(function()
                local l = g.npc.Mesh:GetSocketLocation(FName(g.bone))
                sendGame(string.format("npcdrag %d %s %.1f %.1f %.1f", id, g.bone, l.X, l.Y, l.Z))
            end)
        end
    end
    for id, g in pairs(EX.grab) do
        if not now[id] then
            local vx, vy, vz = 0, 0, 0
            pcall(function() local vel = g.npc.Mesh:GetPhysicsLinearVelocity(FName(g.bone)) vx, vy, vz = vel.X, vel.Y, vel.Z end)
            sendGame(string.format("npcdrop %d %s %.0f %.0f %.0f", id, g.bone, vx, vy, vz))
            EX.dropAt[id] = frameNo
            log(string.format("released NPC %d (%.0f cm/s)", id, math.sqrt(vx * vx + vy * vy + vz * vz)))
        end
    end
    EX.grab = now
end

-- true while this game must not pull NPC `id` back to the other side's position
function EX.locallyControlled(id)
    return EX.grab[id] ~= nil or (EX.dropAt[id] ~= nil and frameNo - EX.dropAt[id] < 90)
end

function EX.remoteGrabTick()
    for id, g in pairs(EX.remoteGrab) do
        local a = npcActor[id]
        if frameNo - g.at > 90 or not valid(a) then
            EX.remoteGrab[id] = nil
        elseif not EX.grab[id] then
            pcall(function()
                local mesh = a.Mesh
                local b = FName(g.bone)
                if not mesh:IsSimulatingPhysics(b) and not g.ragdolled then
                    g.ragdolled = true
                    a:RagdollStart()                 -- grabbing a body turns it into a ragdoll
                end
                local l = mesh:GetSocketLocation(b)
                local ex, ey, ez = g.x - l.X, g.y - l.Y, g.z - l.Z
                mesh:SetPhysicsLinearVelocity({ X = ex * 10, Y = ey * 10, Z = ez * 10 }, false, b)
            end)
        end
    end
end

------------------------------------------------------------------------------- props (level objects)
-- Map objects exist in both games under the same name, so a grabbed prop (bottle, chair, box, a gun
-- lying in the level...) is moved in the other game too: streamed while held and while it flies
-- after a throw, until it comes to rest.
function EX.propTick()
    local held = {}
    for _, side in ipairs({ "L", "R" }) do
        local a = heldState[side].actor
        if valid(a) and not a:IsA(cls(NPC_CLASS_PATH)) and EX.isLevel(a) then held[objPath(a)] = a end
    end
    for path, a in pairs(held) do
        if not EX.props[path] then log("holding a map object: " .. path) end
        EX.props[path] = { actor = a, held = true, still = 0, since = frameNo }
    end
    for path, s in pairs(EX.props) do
        if not held[path] then
            if s.held then s.held, s.since = false, frameNo end
            local done = not valid(s.actor)
            if not done and frameNo % 3 == 0 then
                local vx, vy, vz = 0, 0, 0
                pcall(function() local v = s.actor:GetVelocity() vx, vy, vz = v.X, v.Y, v.Z end)
                s.still = (vx * vx + vy * vy + vz * vz < 9) and s.still + 1 or 0
                if s.still > 10 or frameNo - s.since > 600 then done = true end
            end
            if done then
                if valid(s.actor) then EX.sendProp(path, s.actor) end   -- final resting place
                EX.props[path] = nil
            end
        end
    end
    if frameNo % 3 == 0 then
        for path, s in pairs(EX.props) do if valid(s.actor) then EX.sendProp(path, s.actor) end end
    end
end

function EX.sendProp(path, a)
    pcall(function()
        local l = a:K2_GetActorLocation()
        local r = a:K2_GetActorRotation()
        local qx, qy, qz, qw = rotToQuat(r.Pitch, r.Yaw, r.Roll)
        local vx, vy, vz = 0, 0, 0
        pcall(function() local v = a:GetVelocity() vx, vy, vz = v.X, v.Y, v.Z end)
        sendGame(string.format("prop %s %.1f %.1f %.1f %.4f %.4f %.4f %.4f %.0f %.0f %.0f", path, l.X, l.Y, l.Z, qx, qy, qz, qw, vx, vy, vz))
    end)
end

function EX.onProp(path, x, y, z, qx, qy, qz, qw, vx, vy, vz)
    if EX.props[path] then return end                 -- I'm holding / throwing it myself
    local a = EX.propCache[path]
    if not valid(a) then
        a = StaticFindObject(path)
        if not valid(a) or not EX.isLevel(a) then
            if EX.missingProp ~= path then EX.missingProp = path log("map object not found here: " .. path) end
            return
        end
        EX.propCache[path] = a
    end
    local p, yw, r = quatToRot(qx, qy, qz, qw)
    pcall(function()
        a:K2_SetActorLocationAndRotation({ X = x, Y = y, Z = z }, { Pitch = p, Yaw = yw, Roll = r }, false, {}, true)
        local root = a:K2_GetRootComponent()
        root:SetPhysicsLinearVelocity({ X = vx, Y = vy, Z = vz }, false, FName("None"))
    end)
end

function EX.tick()
    if status.state ~= "Connected" then return end
    if frameNo % 15 == 0 then pcall(EX.readVision) end
    if not syncActive() then return end
    local ok, err = pcall(EX.grabTick)
    if not ok and frameNo % 300 == 0 then log("grab sync error: " .. tostring(err)) end
    pcall(EX.remoteGrabTick)
    ok, err = pcall(EX.propTick)
    if not ok and frameNo % 300 == 0 then log("prop sync error: " .. tostring(err)) end
end

function EX.handle(kind, rest)
    if kind == "vision" then
        local lamp, nv = rest:match("^(%d) (%d)")
        if lamp then
            if tonumber(lamp) ~= EX.remoteVis.lamp or tonumber(nv) ~= EX.remoteVis.nv then
                log(string.format("other player's head lamp %s, night vision %s", lamp == "1" and "on" or "off", nv == "1" and "on" or "off"))
            end
            EX.remoteVis.lamp, EX.remoteVis.nv = tonumber(lamp), tonumber(nv)
        end
    elseif kind == "npcdrag" then
        local id, bone, x, y, z = rest:match("^(%d+) (%S+) (%S+) (%S+) (%S+)")
        id = tonumber(id or "")
        if id then
            local g = EX.remoteGrab[id] or {}
            g.bone, g.x, g.y, g.z, g.at = bone, tonumber(x), tonumber(y), tonumber(z), frameNo
            EX.remoteGrab[id] = g
        end
    elseif kind == "npcdrop" then
        local id, bone, vx, vy, vz = rest:match("^(%d+) (%S+) (%S+) (%S+) (%S+)")
        id = tonumber(id or "")
        if id then
            EX.remoteGrab[id] = nil
            local a = npcActor[id]
            if valid(a) and not EX.grab[id] then
                pcall(function()
                    local v = { X = tonumber(vx), Y = tonumber(vy), Z = tonumber(vz) }
                    a.Mesh:SetPhysicsLinearVelocity(v, false, FName(bone))
                    a.Mesh:SetPhysicsLinearVelocity(v, false, FName("Pelvis"))
                end)
            end
        end
    elseif kind == "prop" then
        local path, x, y, z, qx, qy, qz, qw, vx, vy, vz = rest:match("^(%S+) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+)")
        if path then
            EX.onProp(path, tonumber(x), tonumber(y), tonumber(z), tonumber(qx), tonumber(qy), tonumber(qz), tonumber(qw), tonumber(vx), tonumber(vy), tonumber(vz))
        end
    end
end

------------------------------------------------------------------------------- NPCs attack both players
-- The game's NPC AI can only target its own player (WENDIGOREF is typed as the player pawn), so it
-- always aims at the host. When an NPC on the host fires while the joining player is the closer
-- target, the shot is treated as fired at the joining player: their game plays the gunshot from
-- that NPC, turns the NPC towards them and rolls the hit (better odds up close).
EX.NPC_GUNSHOT = "/Game/Audio/gunfire_remix/Glock/Glock_Fire_Indoor_rmx_Cue.Glock_Fire_Indoor_rmx_Cue"

function EX.installFireHook()
    if EX.fireHooked then return end
    EX.fireHooked = true
    local ok, err = pcall(RegisterHook, NPC_CLASS .. ":Fire_pistol", function(ctx)
        local okF, e = pcall(EX.onNpcFire, ctx:get())
        if not okF then log("NPC shot handling failed: " .. tostring(e)) end
    end)
    log((ok and "hooked " or "could not hook ") .. "NPC Fire_pistol" .. (ok and "" or (": " .. tostring(err))))
end

function EX.onNpcFire(npc)
    if status.role ~= "host" or not syncActive() or not valid(puppet) or puppetDead or not valid(npc) or isPuppet(npc) then return end
    local id = npcIdOf[npc:GetFullName()]
    if not id then return end
    local l = npc:K2_GetActorLocation()
    local p = puppet:K2_GetActorLocation()
    local dc = math.sqrt(dist2(l.X, l.Y, l.Z, p.X, p.Y, p.Z))
    local dh = math.sqrt(dist2(l.X, l.Y, l.Z, localHead[1], localHead[2], localHead[3]))
    if dc > 3000 or dc > dh * 0.85 then return end          -- the host is the closer target
    local chance = math.max(0.12, math.min(0.5, 0.55 - dc / 4000))
    local bone = "miss"
    if math.random() < chance then
        local r = math.random()
        bone = r < 0.1 and "head" or r < 0.75 and "spine_03" or r < 0.875 and "upperarm_l" or "thigh_r"
    end
    sendGame(string.format("npcshot %d %s", id, bone))
end

------------------------------------------------------------------------------- in-headset status
-- Wrist panel: raise your left hand in front of your face and look at it - a small status panel
-- (session, player, ping, maps) pops up above the hand with a quick grow animation.
function EX.wristTick()
    local hx, hy, hz = localHead[1], localHead[2], localHead[3]
    local lx, ly, lz = localLeft[1], localLeft[2], localLeft[3]
    local dx, dy, dz = lx - hx, ly - hy, lz - hz
    local d = math.sqrt(dx * dx + dy * dy + dz * dz)
    local fx, fy, fz = qrot(headQuat[1], headQuat[2], headQuat[3], headQuat[4], 1, 0, 0)
    local looking = d > 15 and d < 55 and (dx * fx + dy * fy + dz * fz) / d > 0.9 and lz < hz
    EX.wristT = EX.wristT or 0
    EX.wristT = looking and math.min(1, EX.wristT + 0.12) or math.max(0, EX.wristT - 0.15)
    if EX.wristT <= 0 then
        if valid(EX.wrist) and not EX.wristHidden then EX.wrist:SetActorHiddenInGame(true) EX.wristHidden = true end
        return
    end
    if not valid(EX.wrist) then
        EX.wrist = textActor("", 2.2, 235, 225, 215)
        if not EX.wrist then return end
    end
    if EX.wristHidden ~= false then EX.wrist:SetActorHiddenInGame(false) EX.wristHidden = false end
    if frameNo % 15 == 0 or EX.wristText == nil then
        local s
        if status.state == "Connected" then
            s = string.format("BLOODTRAIL TOGETHER\n%s  -  %d ms\n%s\nthem: %d%% health", status.peer or "Player",
                math.floor(status.rtt or 0), status.sameMap and "same map" or "different maps", math.floor((remoteHealth or 1) * 100 + 0.5))
        elseif status.state == "Idle" or status.state == "Disconnected" then
            s = "BLOODTRAIL TOGETHER\nnot in a game\npress F9 on the desktop"
        else
            s = "BLOODTRAIL TOGETHER\n" .. tostring(status.state)
        end
        if s ~= EX.wristText then EX.wristText = s EX.wrist.TextRender:K2_SetText(FText(s)) end
    end
    local e = 1 - (1 - EX.wristT) ^ 3                     -- ease-out grow
    local yaw = math.atan(hy - ly, hx - lx) * RAD
    EX.wrist:K2_SetActorLocationAndRotation({ X = lx, Y = ly, Z = lz + 9 }, { Pitch = 0, Yaw = yaw, Roll = 0 }, false, {}, true)
    EX.wrist:SetActorScale3D({ X = e, Y = e, Z = e })
end

-- A note in front of you when the connection stays weak for a few seconds (at most every 30 s).
function EX.connectionWarning()
    if status.state == "Connected" and (status.quality or 3) <= 1 then
        EX.weakFrames = (EX.weakFrames or 0) + 1
        if EX.weakFrames == 270 and frameNo - (EX.warnedAt or -100000) > 2700 then
            EX.warnedAt = frameNo
            showNote(string.format("Weak connection to %s (%d ms)", status.peer or "the other player", math.floor(status.rtt or 0)))
        end
    else
        EX.weakFrames = 0
    end
end

-- my own life, reported to the other player
local myDeadReported = false
local function watchMyHealth()
    local pawn = getPawn()
    if not valid(pawn) then return end
    local okH, hp = pcall(function() return pawn.PlayerHealth end)
    if not okH or type(hp) ~= "number" then return end
    -- my health share for the other player's health bar (sent when it changes by 2 % or more)
    local maxHp = 250
    pcall(function() maxHp = pawn.playerhealthmax end)
    if type(maxHp) ~= "number" or maxHp <= 0 then maxHp = 250 end
    local frac = math.max(0, math.min(1, hp / maxHp))
    if not EX.hpSent or math.abs(frac - EX.hpSent) >= 0.02 or frameNo - (EX.hpAt or 0) > 1800 then
        EX.hpSent, EX.hpAt = frac, frameNo
        sendGame(string.format("hp %.3f", frac))
    end
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
        local frac, bone, causer = rest:match("^(%S+) (%S+) (%S+)")
        frac = tonumber(frac) or 0
        local pawn = getPawn()
        if valid(pawn) and frac > 0 then
            local maxHp = 100
            pcall(function() maxHp = pawn.playerhealthmax end)
            if type(maxHp) ~= "number" or maxHp <= 0 then maxHp = 100 end
            local dmg = frac * maxHp
            log(string.format("shot by the other player: %s (%s), %.0f%% of max health (%.0f)", bone or "?", causer or "?", frac * 100, dmg))
            local ok, err = pcall(function() pawn:BulletDamage(dmg) end)
            if not ok then log("applying damage failed: " .. tostring(err)) end
            -- a killing shot must kill even if the game's damage code softens it
            if frac >= 1.0 then
                pcall(function()
                    if pawn.PlayerHealth > 0 then
                        pawn.PlayerHealth = 0
                        pawn:BulletDamage(maxHp)
                    end
                end)
            end
        end
    elseif kind == "dead" then
        puppetDie()
        showNote((status.peer or "The other player") .. " died")
    elseif kind == "alive" then
        puppetRespawn()
    elseif kind == "held" then
        -- held <L|R> <part count> <scale of the gripped component>
        local side, n, ms = rest:match("^([LR]) (%d+) (%S+)")
        if side then
            local old = remoteHeld[side]
            remoteHeld[side] = { ver = old.ver + 1, n = tonumber(n), ms = tonumber(ms) or 1, parts = {} }
        end
    elseif kind == "heldpart" then
        -- heldpart <L|R> <i> <mesh path> <K|S> <rel x y z> <rel quat x y z w> <scale>
        local side, i, path, k, a, b, c, d, e, f, g, s = rest:match("^([LR]) (%d+) (%S+) ([KS]) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+) (%S+)")
        if side then
            remoteHeld[side].parts[tonumber(i)] = { path = path, kind = k, x = tonumber(a), y = tonumber(b), z = tonumber(c),
                q = { tonumber(d), tonumber(e), tonumber(f), tonumber(g) }, s = tonumber(s) or 1 }
        end
    elseif kind == "npc" or kind == "npcgone" or kind == "npchit" or kind == "npcbody" then
        handleNpcEvent(kind, rest)
    elseif kind == "hp" then
        remoteHealth = tonumber(rest) or remoteHealth
    elseif kind == "npcshot" then
        -- an NPC on the host fired at me: gunshot from that NPC, turn it towards me, maybe a hit
        local id, bone = rest:match("^(%d+) (%S+)")
        id = tonumber(id or "")
        local m = id and npcActor[id]
        if valid(m) then
            local l = m:K2_GetActorLocation()
            playRemoteSound(EX.NPC_GUNSHOT, l.X, l.Y, l.Z + 50, 1, 1)
            if mirrorTarget[id] then mirrorTarget[id].aimUntil = frameNo + 60 end
        end
        if bone and bone ~= "miss" then
            handleGame(string.format("hit %.2f %s npc_9mm", damageForHit(bone, "ammo_9mm_b") * 0.6, bone))
        end
    elseif kind == "vision" or kind == "npcdrag" or kind == "npcdrop" or kind == "prop" then
        EX.handle(kind, rest)
    end
end

local handshakeDone = false
local framePending = false

local function parseReply(reply)
    for line in reply:gmatch("[^\n]+") do
        local kind = line:sub(1, 2)
        if kind == "ST" then
            local st, role, q, rtt, loss, sim, code, peer, same = line:match("^ST (%S+) (%S+) (%d+) (%S+) (%S+) (%d) (%S+) (%S+) ?(%d?)")
            if st then
                status.state, status.role, status.quality, status.rtt = st, role, tonumber(q), tonumber(rtt) or 0
                status.sameMap = same == "1"
                status.simulated = sim == "1"
                status.peer = (peer ~= "-") and peer or "Player"
                status.code = code
            end
        elseif kind == "RP" then
            if line == "RP -" then
                hideAvatar()
                if valid(puppet) and not puppetDead then destroyPuppet() end
                destroyHeld()
                pcall(EX.hideRemoteLights)
            else
                local v = {}
                for num in line:sub(4):gmatch("%S+") do v[#v + 1] = tonumber(num) end
                if #v >= 43 then
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
            local radius, _, st, npc = line:match("^CFG (%S+) (%S+) ?(%S*) ?(%d?)")
            npcSyncEnabled = npc ~= "0"
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
    for i = 1, #heldEvents do req[#req + 1] = "X " .. heldEvents[i] end
    heldEvents = {}
    if frameNo % MAP_POLL_FRAMES == 0 or currentMap == nil then
        local okM, m = pcall(currentMapPath)
        if okM and m and m ~= currentMap then
            currentMap = m
            travelling = false
            avatarDisabled = false   -- try again on the new map
            destroyAvatar()
            note = nil
            puppet, puppetLast, puppetDead, puppetFailed, pup = nil, nil, false, false, nil  -- old body died with the old map
            heldShown = { L = nil, R = nil }
        end
        if currentMap then req[#req + 1] = "M " .. currentMap end
    end
    if frameNo % 900 == 0 then
        req[#req + 1] = string.format("D sound hooks fired=%d captured=%d played=%d missing=%d avatar=%s",
            hookCalls, soundsCaptured, soundsPlayed, soundsMissing, tostring(avatar ~= nil and not avatarDisabled))
    end
    for i = 1, #pendingSounds do req[#req + 1] = pendingSounds[i] end
    pendingSounds = {}
    if frameNo % 10 == 0 then pcall(watchMyHealth) end
    -- NPC sync (config.ini [Gameplay] NpcSync=1, the default). The simulated player runs no game.
    if not status.simulated and npcSyncEnabled then
        local okN, errN = pcall(npcSyncTick)
        if not okN and frameNo % 300 == 0 then log("NPC sync error: " .. tostring(errN)) end
    end
    -- torch / night vision, dragging bodies, thrown props
    if not status.simulated then
        local okE, errE = pcall(EX.tick)
        if not okE and frameNo % 300 == 0 then log("extras sync error: " .. tostring(errE)) end
    end
    -- developer self-test: shoot the other player's body with a point-damage event
    -- developer self-test: go to the Sandbox map by itself (host side of NPC sync)
    if selfTest:find("sandbox", 1, true) and frameNo == 600 and currentMap and not currentMap:find("Map_Sandbox", 1, true) then
        travelTo("/Game/Maps/Sandbox/Map_Sandbox")
    end
    -- developer self-test: shoot the first mirrored NPC (client side of NPC sync)
    if selfTest:find("npchit", 1, true) and frameNo % 450 == 0 then
        for _, m in pairs(npcActor) do
            if valid(m) and status.role == "client" then
                local okT, errT = pcall(function()
                    local hl = m.Mesh:GetSocketLocation(FName("spine_03"))
                    local ok, why = applyBulletHit(m, "spine_03", hl.X, hl.Y, hl.Z, -1, 0, 0, "ammo_9mm_b", false)
                    log("self-test: bullet into a mirrored NPC: " .. tostring(ok) .. " " .. tostring(why or ""))
                end)
                if not okT then log("self-test failed: " .. tostring(errT)) end
                break
            end
        end
    end
    if selfTest:find("hit", 1, true) == 1 and valid(puppet) and frameNo % 600 == 0 then
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
    pcall(EX.wristTick)
    pcall(EX.connectionWarning)
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
