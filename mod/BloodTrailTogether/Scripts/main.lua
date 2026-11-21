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
local function valid(o) return o ~= nil and o.IsValid ~= nil and o:IsValid() end

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
    return string.format("P %.2f %.2f %.2f %.5f %.5f %.5f %.5f %.2f %.2f %.2f %.5f %.5f %.5f %.5f %.2f %.2f %.2f %.5f %.5f %.5f %.5f %d",
        hx, hy, hz, qx, qy, qz, qw, L[1], L[2], L[3], L[4], L[5], L[6], L[7], R[1], R[2], R[3], R[4], R[5], R[6], R[7], flags)
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
local function updateAvatar(v, peerName, quality, rtt)
    if avatarDisabled then return end
    if not ensureAvatar(peerName) then
        avatarDisabled = true
        return
    end
    if avatar.hidden then
        for k, a in pairs(avatar) do
            if k ~= "hidden" and valid(a) then a:SetActorHiddenInGame(false) end
        end
        avatar.hidden = false
    end
    -- head
    place(avatar.head, v[1], v[2], v[3], v[4], v[5], v[6], v[7])
    local fx, fy, fz = qrot(v[4], v[5], v[6], v[7], 9, 0, 0)       -- visor 9 cm in front of the head centre
    place(avatar.visor, v[1] + fx, v[2] + fy, v[3] + fz, v[4], v[5], v[6], v[7])
    place(avatar.left, v[8], v[9], v[10], v[11], v[12], v[13], v[14])
    place(avatar.right, v[15], v[16], v[17], v[18], v[19], v[20], v[21])
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

local function captureSound(sound, x, y, z, vol, pitch)
    if playingRemote or #pendingSounds > 16 then return end
    if not valid(sound) or not nearLocalPlayer(x, y, z) then return end
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
    if not snd then return end
    local world = UEHelpers.GetWorld()
    local gs = UEHelpers.GetGameplayStatics()
    playingRemote = true
    pcall(function()
        gs:PlaySoundAtLocation(world, snd, { X = x, Y = y, Z = z }, { Pitch = 0, Yaw = 0, Roll = 0 }, vol, pitch, 0, nil, nil, nil)
    end)
    playingRemote = false
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
local handshakeDone = false
local status = { state = "Idle" }
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
            else
                local v = {}
                for num in line:sub(4):gmatch("%S+") do v[#v + 1] = tonumber(num) end
                if #v >= 21 then
                    local ok, err = pcall(updateAvatar, v, status.peer or "Player", status.quality or 0, math.floor(status.rtt or 0))
                    if not ok then log("avatar update failed: " .. tostring(err)) avatarDisabled = true end
                end
            end
        elseif kind == "SN" then
            local path, x, y, z, vol, pitch = line:match("^SN (%S+) (%S+) (%S+) (%S+) (%S+) (%S+)")
            if path then playRemoteSound(path, tonumber(x), tonumber(y), tonumber(z), tonumber(vol), tonumber(pitch)) end
        elseif kind == "TR" then
            travelTo(line:sub(4))
        elseif kind == "NT" then
            showNote(line:sub(4))
        elseif kind == "CF" then
            local radius = line:match("^CFG (%S+)")
            if radius then shareRadius2 = tonumber(radius) ^ 2 end
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
        end
        if currentMap then req[#req + 1] = "M " .. currentMap end
    end
    for i = 1, #pendingSounds do req[#req + 1] = pendingSounds[i] end
    pendingSounds = {}

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

LoopAsync(FRAME_MS, function()
    if not framePending then
        framePending = true
        ExecuteInGameThread(function()
            local ok, err = pcall(frame)
            if not ok then log("frame error: " .. tostring(err)) end
        end)
    end
    return false  -- keep looping
end)

log("BloodTrail Together " .. VERSION .. " Lua side started (F9 toggles the multiplayer window)")
