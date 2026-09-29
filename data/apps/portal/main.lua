-- Portal — watch face is a window into a fixed starship bay.
-- IMU aims the window (yaw/pitch/roll); the room stays put.
-- Voice "shoot"/"pick" is out of scope (see docs/PORTAL.md).
--
-- v1.0.2 crash fix: gfx_clear + 16 wall strips + shared ceil/floor (≤~20 LVGL objs).
-- v1.0.3: tip-up/face-up → ceiling (+pitch via asin(az)).
-- v1.1.0: orientation window —
--   yaw   = pan around room (gyro ∫ ω·ĝ; accel atan(ay, ax) fallback when flat)
--   pitch = tip up/down (accel gravity; keep 1.0.3 tip-up=look-up)
--   roll  = bank / twist (accel); shear wall strips so horizon tilts
-- IMU fields: watch.imu → ax,ay,az (g) + gx,gy,gz (dps) from QMI8658 / sim.

local W = watch.width()
local H = watch.height()
local OX, OY = 8, 40
local VW = W - 16
local VH = H - 100

local COLS = 16
local COL_W = math.max(2, math.floor(VW / COLS))
local FOV = math.pi / 3
local MAP_W, MAP_H = 12, 12
local DEG2RAD = math.pi / 180

local map = {
  "############",
  "#..........#",
  "#..##..##..#",
  "#..........#",
  "#....##....#",
  "#..........#",
  "#..#....#..#",
  "#..#....#..#",
  "#..........#",
  "#....##....#",
  "#..........#",
  "############",
}

local pos_x, pos_y = 6.0, 6.0
local yaw = 0.0
local pitch = -0.15
local roll = 0.0

local bias_yaw = 0.0
local bias_pitch = 0.0
local bias_roll = 0.0
local bias_gx, bias_gy, bias_gz = 0.0, 0.0, 0.0

local mode = "menu"  -- menu | calib | look
local status = "tip to look"
local have_imu = false
local have_gyro = false
local calib_n, calib_ticks = 0, 0
local calib_sy, calib_sp, calib_sr = 0.0, 0.0, 0.0
local calib_sgx, calib_sgy, calib_sgz = 0.0, 0.0, 0.0
local CALIB_SAMPLES = 4
local CALIB_MAX_TICKS = 12
local frame = 0
local last_ms = nil
local last_yaw, last_pitch, last_roll = nil, nil, nil
local have_rect = (watch.rect ~= nil)
local have_gfx_clear = (watch.gfx_clear ~= nil)

-- Complementary-lite: blend toward accel absolute for pitch/roll
local ACC_BLEND = 0.28
local GYRO_LIVE_DPS = 0.8
local GYRO_DEAD_DPS = 0.15
-- Flat enough for accel heading yaw fallback (|az| after normalize)
local FLAT_AZ = 0.55

local function clampf(v, a, b)
  if v < a then return a end
  if v > b then return b end
  return v
end

local function solid(mx, my)
  if mx < 0 or my < 0 or mx >= MAP_W or my >= MAP_H then
    return true
  end
  local row = map[my + 1]
  if not row then return true end
  return row:sub(mx + 1, mx + 1) == "#"
end

-- Accel → absolute pitch/roll (gravity). Pitch sign matches v1.0.3.
local function accel_pr(ax, ay, az)
  local n = math.sqrt(ax * ax + ay * ay + az * az)
  if n < 0.05 then n = 1 end
  ax, ay, az = ax / n, ay / n, az / n
  -- tip-up / face-up (az→+1) → +pitch (ceiling); face-down → −pitch (floor)
  local p = math.asin(clampf(az, -1, 1))
  -- tip left/right banks the horizon
  local r = math.atan(ay, az)
  return p, r, ax, ay, az
end

local function accel_heading(ax, ay)
  return math.atan(ay, ax)
end

-- Complementary / Madgwick-lite orientation step.
-- Gyro (dps) integrated about gravity for yaw/pan; accel anchors pitch+roll.
local function imu_update(imu, dt)
  if not imu then return end
  local ax = imu.ax or 0
  local ay = imu.ay or 0
  local az = imu.az or 1
  local gx = (imu.gx or 0) - bias_gx
  local gy = (imu.gy or 0) - bias_gy
  local gz = (imu.gz or 0) - bias_gz

  local p_a, r_a, nax, nay, naz = accel_pr(ax, ay, az)
  p_a = p_a - bias_pitch
  r_a = r_a - bias_roll

  local gmag = math.abs(gx) + math.abs(gy) + math.abs(gz)
  if gmag > GYRO_LIVE_DPS then
    have_gyro = true
  end

  if dt < 0.001 then dt = 0.001 end
  if dt > 0.25 then dt = 0.25 end

  if have_gyro then
    -- yaw rate = body ω projected on gravity (pan about vertical)
    local gx_r = gx * DEG2RAD
    local gy_r = gy * DEG2RAD
    local gz_r = gz * DEG2RAD
    if gmag > GYRO_DEAD_DPS then
      local yaw_rate = gx_r * nax + gy_r * nay + gz_r * naz
      yaw = yaw + yaw_rate * dt
    end
    -- pitch/roll: gyro-smoothed toward accel absolute
    pitch = pitch + ACC_BLEND * (p_a - pitch)
    roll = roll + ACC_BLEND * (r_a - roll)
  else
    -- Accel-only: pitch/roll absolute; yaw from heading when mostly flat
    pitch = p_a
    roll = r_a
    if math.abs(naz) > FLAT_AZ then
      yaw = accel_heading(nax, nay) - bias_yaw
    end
  end

  pitch = clampf(pitch, -1.25, 1.25)
  roll = clampf(roll, -1.2, 1.2)
end

local function shade(r, g, b, dist, side)
  local f = 1.0 / (1.0 + dist * 0.22)
  if side == 1 then f = f * 0.72 end
  return math.floor(r * f), math.floor(g * f), math.floor(b * f)
end

local function wall_col(side, dist, mx, my)
  local edge = (mx <= 0 or mx >= MAP_W - 1 or my <= 0 or my >= MAP_H - 1)
  local r, g, b
  if edge then
    r, g, b = 70, 95, 120
  else
    r, g, b = 120, 95, 55
  end
  if (mx + my) % 3 == 0 then
    r = math.floor(r * 0.85)
    g = math.floor(g * 0.85)
    b = math.floor(b * 0.85)
  end
  return shade(r, g, b, dist, side)
end

local function cast_column(col)
  local cam = (col + 0.5) / COLS * 2 - 1
  local ray_a = yaw + cam * (FOV * 0.5)
  local ray_dx = math.cos(ray_a)
  local ray_dy = math.sin(ray_a)

  local map_x = math.floor(pos_x)
  local map_y = math.floor(pos_y)

  local delta_x = (ray_dx == 0) and 1e30 or math.abs(1 / ray_dx)
  local delta_y = (ray_dy == 0) and 1e30 or math.abs(1 / ray_dy)

  local step_x, side_dist_x
  if ray_dx < 0 then
    step_x = -1
    side_dist_x = (pos_x - map_x) * delta_x
  else
    step_x = 1
    side_dist_x = (map_x + 1.0 - pos_x) * delta_x
  end
  local step_y, side_dist_y
  if ray_dy < 0 then
    step_y = -1
    side_dist_y = (pos_y - map_y) * delta_y
  else
    step_y = 1
    side_dist_y = (map_y + 1.0 - pos_y) * delta_y
  end

  local hit = false
  local side = 0
  for _ = 1, 20 do
    if side_dist_x < side_dist_y then
      side_dist_x = side_dist_x + delta_x
      map_x = map_x + step_x
      side = 0
    else
      side_dist_y = side_dist_y + delta_y
      map_y = map_y + step_y
      side = 1
    end
    if solid(map_x, map_y) then
      hit = true
      break
    end
  end

  local dist
  if hit then
    if side == 0 then
      dist = (map_x - pos_x + (1 - step_x) / 2) / ray_dx
    else
      dist = (map_y - pos_y + (1 - step_y) / 2) / ray_dy
    end
  else
    dist = 20
  end
  if dist < 0.05 then dist = 0.05 end

  return dist, side, map_x, map_y, hit
end

local function safe_rect(x, y, w, h, r, g, b)
  if not have_rect then return end
  if w < 1 or h < 1 then return end
  watch.rect(x, y, w, h, r, g, b)
end

local function render_view(force)
  if not have_rect then
    if watch.set_text then
      watch.set_text("no rect API")
    end
    return
  end

  if not force and last_yaw and last_pitch and last_roll then
    local dy = yaw - last_yaw
    local dp = pitch - last_pitch
    local dr = roll - last_roll
    if dy < 0 then dy = -dy end
    if dp < 0 then dp = -dp end
    if dr < 0 then dr = -dr end
    if dy < 0.025 and dp < 0.025 and dr < 0.02 then
      return
    end
  end
  last_yaw, last_pitch, last_roll = yaw, pitch, roll

  -- CRITICAL: each watch.rect = new LVGL obj (no reuse, no null check in FW).
  if have_gfx_clear then
    watch.gfx_clear()
  end

  local horizon = math.floor(VH / 2 + pitch * (VH / math.pi))
  horizon = clampf(horizon, 8, VH - 8)

  if watch.fill then
    watch.fill(8, 10, 16)
  end

  -- Shared ceil / floor at mean horizon (2 objs). Roll shears per-column walls.
  safe_rect(OX, OY, VW, horizon, 28, 22, 40)
  safe_rect(OX, OY + horizon, VW, VH - horizon, 32, 36, 42)

  local mid = (COLS - 1) * 0.5
  if mid < 1 then mid = 1 end
  -- Edge shear ≈ roll * 0.35 * VH  (bank the view without extra LVGL objs)
  local shear_amp = roll * (VH * 0.35)

  for col = 0, COLS - 1 do
    local dist, side, mx, my, hit = cast_column(col)
    local line_h = math.floor(VH / dist)
    if line_h > VH * 2 then line_h = VH * 2 end

    local shear = ((col - mid) / mid) * shear_amp
    local hzn = clampf(math.floor(horizon + shear), 2, VH - 2)

    local top = hzn - math.floor(line_h / 2)
    local bot = hzn + math.floor(line_h / 2)
    if top < 0 then top = 0 end
    if bot > VH then bot = VH end

    local x = OX + col * COL_W
    local w = COL_W
    if col == COLS - 1 then
      w = VW - col * COL_W
    end

    if bot > top then
      if hit then
        local wr, wg, wb = wall_col(side, dist, mx, my)
        safe_rect(x, OY + top, w, bot - top, wr, wg, wb)
      else
        safe_rect(x, OY + top, w, bot - top, 16, 18, 28)
      end
    end
  end

  -- Horizon tick at mean pitch (roll is visible via strip shear)
  if horizon > 2 and horizon < VH - 2 then
    safe_rect(OX, OY + horizon, VW, 1, 40, 50, 60)
  end
end

local function set_status(s)
  status = s
  if watch.set_text then
    watch.set_text(status)
  end
end

local function status_look()
  local gtag = have_gyro and "gyro" or "accel"
  return string.format(
    "yaw %.2f  pitch %.2f  roll %.2f  [%s]",
    yaw, pitch, roll, gtag
  )
end

local function build_look_ui(is_calib)
  if watch.clear_ui then watch.clear_ui() end
  if watch.set_title then watch.set_title("Portal") end
  last_yaw, last_pitch, last_roll = nil, nil, nil
  render_view(true)
  if is_calib then
    set_status("Hold still…")
  else
    set_status(status)
  end
end

local function show_menu()
  mode = "menu"
  last_yaw, last_pitch, last_roll = nil, nil, nil
  last_ms = nil
  if watch.clear_ui then watch.clear_ui() end
  if watch.button_layout then watch.button_layout("column") end
  if watch.set_title then watch.set_title("Portal") end
  if watch.fill then watch.fill(6, 8, 14) end
  if watch.set_text then
    watch.set_text(
      "Starship window\n\n" ..
      "Yaw  — pan left/right\n" ..
      "Pitch — tip up/down\n" ..
      "Roll — twist / bank\n\n" ..
      "Tip up → ceiling\n" ..
      "Gyro pans when available\n\n" ..
      "Voice shoot/pick: later"
    )
  end
  if watch.button then
    watch.button("Look", function()
      bias_yaw, bias_pitch, bias_roll = 0, 0, 0
      bias_gx, bias_gy, bias_gz = 0, 0, 0
      calib_n, calib_ticks = 0, 0
      calib_sy, calib_sp, calib_sr = 0, 0, 0
      calib_sgx, calib_sgy, calib_sgz = 0, 0, 0
      have_imu = false
      have_gyro = false
      yaw, pitch, roll = 0, -0.15, 0
      last_ms = nil
      mode = "calib"
      status = "Hold still…"
      build_look_ui(true)
    end)
  end
end

if watch.on_back then
  watch.on_back(function()
    if mode == "look" or mode == "calib" then
      show_menu()
    else
      if watch.back then watch.back() end
    end
  end)
end

if watch.on_tick then
  watch.on_tick(function()
    if mode ~= "calib" and mode ~= "look" then return end
    frame = frame + 1

    local now = 0
    if watch.millis then
      now = watch.millis()
    end
    local dt = 0.05
    if last_ms and now > last_ms then
      dt = (now - last_ms) / 1000.0
    end
    last_ms = now

    local imu = nil
    if watch.imu then
      local ok, res = pcall(watch.imu)
      if ok then imu = res end
    end

    if mode == "calib" then
      calib_ticks = calib_ticks + 1
      if imu then
        have_imu = true
        local p_a, r_a, nax, nay, naz = accel_pr(imu.ax or 0, imu.ay or 0, imu.az or 1)
        local h = accel_heading(nax, nay)
        calib_sy = calib_sy + h
        calib_sp = calib_sp + p_a
        calib_sr = calib_sr + r_a
        calib_sgx = calib_sgx + (imu.gx or 0)
        calib_sgy = calib_sgy + (imu.gy or 0)
        calib_sgz = calib_sgz + (imu.gz or 0)
        local gmag = math.abs(imu.gx or 0) + math.abs(imu.gy or 0) + math.abs(imu.gz or 0)
        calib_n = calib_n + 1
        if gmag > GYRO_LIVE_DPS then
          have_gyro = true
        end
      end
      if calib_n >= CALIB_SAMPLES or calib_ticks >= CALIB_MAX_TICKS then
        if calib_n > 0 then
          bias_yaw = calib_sy / calib_n
          bias_pitch = 0  -- pitch stays absolute (tip-up = ceiling)
          bias_roll = calib_sr / calib_n
          bias_gx = calib_sgx / calib_n
          bias_gy = calib_sgy / calib_n
          bias_gz = calib_sgz / calib_n
        end
        yaw, pitch, roll = 0, -0.15, 0
        if imu then
          local p_a, r_a = accel_pr(imu.ax or 0, imu.ay or 0, imu.az or 1)
          pitch = clampf(p_a - bias_pitch, -1.25, 1.25)
          roll = clampf(r_a - bias_roll, -1.2, 1.2)
        end
        mode = "look"
        if have_imu then
          status = "aim the window"
        else
          status = "no IMU — still view"
        end
      end
      -- Calib: redraw once at start + once when entering look
      if mode == "look" then
        render_view(true)
        set_status(status_look())
      elseif calib_ticks == 1 then
        render_view(true)
        set_status("Hold still…")
      end
      return
    end

    if imu then
      imu_update(imu, dt)
    end
    render_view(false)
    if frame % 5 == 0 then
      set_status(status_look())
    end
  end)
end

show_menu()
print("Portal ready v1.1.0")
