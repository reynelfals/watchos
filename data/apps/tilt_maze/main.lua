-- Tilt Maze - tip the watch to roll the ball (BB labyrinth)
-- Paint maze once; move one persistent watch.ball each tick. ASCII only.
-- Smooth physics: fixed STEP=0.02 substeps, GAIN~100, clamp Δpos/substep.
-- Axis: ax = -(imu.ay - bias) so +ay → LEFT (sapphire HW X fix); ay = +(imu.ax - bias) so +ax → DOWN.
-- v1.1.2: tilt-only play (no Nudge/Reset chrome covering the maze).
-- v1.1.3: floor ball sprite coords — float physics + luaL_checkinteger froze ball.
-- v1.1.4: negate horizontal IMU map — physical watch X was inverted.

local BALL_R = 10
local GOAL_R = 16
local WALL_C = {40, 90, 140}
local FLOOR_C = {8, 12, 18}
local BALL_C = {220, 200, 60}
local GOAL_C = {40, 180, 90}

-- Playfield: reclaim former button column (~410x502 face, ~40/56 insets)
local OX, OY = 20, 48
local PW, PH = 370, 398

local walls = {
  {0, 0, PW, 8},
  {0, PH - 8, PW, 8},
  {0, 0, 8, PH},
  {PW - 8, 0, 8, PH},
  {8, 70, 220, 10},
  {140, 70, 10, 100},
  {8, 160, 100, 10},
  {200, 130, 162, 10},
  {250, 8, 10, 90},
  {60, 200, 10, 150},
  {60, 280, 180, 10},
  {230, 200, 10, 150},
  {120, 340, 200, 10},
  {300, 160, 10, 190},
  {8, 340, 70, 10},
}

local goal = {x = PW - 40, y = PH - 40}
local start = {x = 36, y = 36}

local ball = {x = start.x, y = start.y, vx = 0, vy = 0}
local mode = "menu"  -- menu | calib | play | win
local last_ms = 0
local status = "tip the watch"
local phys_accum = 0

-- Calib / play tuning (smooth substeps — NOT high GAIN + big dt)
local CALIB_SAMPLES = 4
local CALIB_MAX_TICKS = 12
local DEADZONE = 0.04
local GAIN = 100
local MAX_SPEED = 120
local GRACE_FRAMES = 2
local STEP = 0.02          -- fixed physics step (50 Hz)
local MAX_SUBSTEPS = 8     -- cap catch-up (~160ms)
local MAX_DPOS = 4.0       -- max px per substep (anti-teleport)
local calib_n = 0
local calib_ticks = 0
local calib_sx, calib_sy = 0, 0
local bias_x, bias_y = 0, 0
local grace = 0
local have_imu = false

local function clampf(v, a, b)
  if v < a then return a end
  if v > b then return b end
  return v
end

local function circle_hits_aabb(cx, cy, r, rx, ry, rw, rh)
  local qx = clampf(cx, rx, rx + rw)
  local qy = clampf(cy, ry, ry + rh)
  local dx, dy = cx - qx, cy - qy
  return (dx * dx + dy * dy) < (r * r)
end

local function resolve_walls()
  for i = 1, #walls do
    local w = walls[i]
    if circle_hits_aabb(ball.x, ball.y, BALL_R, w[1], w[2], w[3], w[4]) then
      local left = (ball.x + BALL_R) - w[1]
      local right = (w[1] + w[3]) - (ball.x - BALL_R)
      local top = (ball.y + BALL_R) - w[2]
      local bot = (w[2] + w[4]) - (ball.y - BALL_R)
      local m = math.min(left, right, top, bot)
      if m == left then
        ball.x = w[1] - BALL_R
        ball.vx = -math.abs(ball.vx) * 0.25
      elseif m == right then
        ball.x = w[1] + w[3] + BALL_R
        ball.vx = math.abs(ball.vx) * 0.25
      elseif m == top then
        ball.y = w[2] - BALL_R
        ball.vy = -math.abs(ball.vy) * 0.25
      else
        ball.y = w[2] + w[4] + BALL_R
        ball.vy = math.abs(ball.vy) * 0.25
      end
    end
  end
  ball.x = clampf(ball.x, BALL_R + 8, PW - BALL_R - 8)
  ball.y = clampf(ball.y, BALL_R + 8, PH - BALL_R - 8)
end

local function in_goal()
  local dx = ball.x - goal.x
  local dy = ball.y - goal.y
  return (dx * dx + dy * dy) < ((GOAL_R - 4) * (GOAL_R - 4))
end

local function paint_maze_static()
  watch.fill(FLOOR_C[1], FLOOR_C[2], FLOOR_C[3])
  watch.rect(OX, OY, PW, PH, 12, 18, 28)
  for i = 1, #walls do
    local w = walls[i]
    watch.rect(OX + w[1], OY + w[2], w[3], w[4], WALL_C[1], WALL_C[2], WALL_C[3])
  end
  watch.circle(OX + goal.x, OY + goal.y, GOAL_R, GOAL_C[1], GOAL_C[2], GOAL_C[3])
end

local function pix(n)
  -- Round to int for watch.ball / older firmware that used luaL_checkinteger.
  return math.floor((n or 0) + 0.5)
end

local function move_ball_sprite()
  watch.ball(pix(OX + ball.x), pix(OY + ball.y), BALL_R, BALL_C[1], BALL_C[2], BALL_C[3])
end

local function reset_ball_state()
  ball.x, ball.y = start.x, start.y
  ball.vx, ball.vy = 0, 0
  calib_n = 0
  calib_ticks = 0
  calib_sx, calib_sy = 0, 0
  bias_x, bias_y = 0, 0
  grace = GRACE_FRAMES
  have_imu = false
  phys_accum = 0
end

-- Map IMU → screen accel. Sapphire: negate horizontal so tip-right → ball RIGHT.
-- +imu.ax (tilt pad down) → +ay → ball DOWN (Y unchanged).
local function imu_to_accel(imu)
  -- v1.1.4: negate horizontal — physical Waveshare/sapphire X was inverted
  -- +imu.ax (pad down) with Y invert → +screen Y (down)
  local ax = -((imu.ay or 0) - bias_x)
  local ay = ((imu.ax or 0) - bias_y)  -- tip-down → ball down (screen +Y)
  if math.abs(ax) < DEADZONE then ax = 0 end
  if math.abs(ay) < DEADZONE then ay = 0 end
  return ax * GAIN, ay * GAIN
end

local function phys_substep(acc_x, acc_y)
  local ox, oy = ball.x, ball.y
  ball.vx = ball.vx + acc_x * STEP
  ball.vy = ball.vy + acc_y * STEP
  ball.vx = ball.vx * 0.985
  ball.vy = ball.vy * 0.985
  local sp = math.sqrt(ball.vx * ball.vx + ball.vy * ball.vy)
  if sp > MAX_SPEED then
    ball.vx = ball.vx * (MAX_SPEED / sp)
    ball.vy = ball.vy * (MAX_SPEED / sp)
  end
  local dx = ball.vx * STEP
  local dy = ball.vy * STEP
  dx = clampf(dx, -MAX_DPOS, MAX_DPOS)
  dy = clampf(dy, -MAX_DPOS, MAX_DPOS)
  ball.x = ball.x + dx
  ball.y = ball.y + dy
  resolve_walls()
  -- If wall resolve teleported, keep soft by not exploding velocity further
  if math.abs(ball.x - ox) > MAX_DPOS * 2 then
    ball.vx = ball.vx * 0.5
  end
  if math.abs(ball.y - oy) > MAX_DPOS * 2 then
    ball.vy = ball.vy * 0.5
  end
end

local function integrate_physics(elapsed, acc_x, acc_y)
  if elapsed < 0 then elapsed = 0 end
  if elapsed > 0.25 then elapsed = 0.25 end  -- discard huge stalls; substeps handle the rest
  phys_accum = phys_accum + elapsed
  local n = 0
  while phys_accum >= STEP and n < MAX_SUBSTEPS do
    phys_substep(acc_x, acc_y)
    phys_accum = phys_accum - STEP
    n = n + 1
  end
  if n >= MAX_SUBSTEPS then
    phys_accum = 0  -- drop leftover after catch-up cap
  end
  return n
end

local function build_play_screen(is_win)
  watch.clear_ui()
  watch.set_title("Tilt Maze")
  paint_maze_static()
  move_ball_sprite()
  if is_win then
    watch.button_layout("column")
    watch.set_text("YOU WIN!")
    watch.button("Again", function()
      reset_ball_state()
      mode = "calib"
      status = "Hold still"
      last_ms = watch.millis()
      build_play_screen(false)
    end)
  else
    -- Zero buttons during play — native swipe-back / OS back only.
    watch.set_text(status)
  end
end

function show_menu()
  mode = "menu"
  watch.clear_ui()
  watch.button_layout("column")
  watch.set_title("Tilt Maze")
  watch.fill(6, 10, 16)
  watch.set_text(
    "BB labyrinth\n\n" ..
    "Tip the watch to roll\n" ..
    "the ball to the green\n" ..
    "hole. Avoid walls.\n\n" ..
    "Tilt / IMU only."
  )
  watch.button("Play", function()
    reset_ball_state()
    mode = "calib"
    status = "Hold still"
    last_ms = watch.millis()
    build_play_screen(false)
  end)
end

watch.on_tick(function()
  if mode ~= "calib" and mode ~= "play" and mode ~= "win" then return end

  local now = watch.millis()
  local elapsed = (now - last_ms) / 1000.0
  if elapsed < 0 then elapsed = 0 end
  last_ms = now

  if mode == "calib" then
    calib_ticks = calib_ticks + 1
    local imu = watch.imu()
    if imu then
      have_imu = true
      -- Same axis map as play (for consistent bias)
      local ax = (imu.ay or 0)
      local ay = (imu.ax or 0)
      calib_sx = calib_sx + ax
      calib_sy = calib_sy + ay
      calib_n = calib_n + 1
    end
    if calib_n >= CALIB_SAMPLES or calib_ticks >= CALIB_MAX_TICKS then
      if calib_n > 0 then
        bias_x = calib_sx / calib_n
        bias_y = calib_sy / calib_n
      else
        bias_x, bias_y = 0, 0
      end
      mode = "play"
      if have_imu then
        status = "roll to green"
      else
        status = "tip the watch"
      end
      watch.set_text(status)
      grace = GRACE_FRAMES
      phys_accum = 0
      last_ms = watch.millis()
    end
    move_ball_sprite()
    return
  end

  if mode == "play" then
    if grace > 0 then
      grace = grace - 1
      phys_accum = 0
    else
      local acc_x, acc_y = 0, 0
      local imu = watch.imu()
      if imu then
        acc_x, acc_y = imu_to_accel(imu)
      end
      integrate_physics(elapsed, acc_x, acc_y)
      if in_goal() then
        mode = "win"
        status = "YOU WIN!"
        watch.vibrate()
        build_play_screen(true)
        return
      end
    end
  end

  if mode == "play" or mode == "win" then
    move_ball_sprite()
  end
end)

show_menu()
print("Tilt Maze ready v1.1.4")
