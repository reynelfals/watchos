-- Squish ID - offline catalog search + details with WRGB image
-- Letter pad search; Open1/2/3 opens details for the current page row.

local query = ""
local page = 0
local PAGE_SIZE = 3
local results = {}
local mode = "search"  -- "search" | "details"
local detail = nil

local inset = 24
if watch.safe_inset then
  inset = watch.safe_inset() or inset
end

local function price_of(r)
  local p = r.retail_price_usd
  if not p or p == "" then return "-" end
  return "$" .. p
end

local function trunc(s, n)
  if not s then return "" end
  if #s <= n then return s end
  return string.sub(s, 1, n - 3) .. "..."
end

local function page_rows()
  local n = #results
  if n == 0 then return {} end
  local pages = math.floor((n - 1) / PAGE_SIZE) + 1
  if page >= pages then page = pages - 1 end
  if page < 0 then page = 0 end
  local start = page * PAGE_SIZE + 1
  local last = math.min(start + PAGE_SIZE - 1, n)
  local rows = {}
  for i = start, last do
    table.insert(rows, results[i])
  end
  return rows
end

local function placeholder(name, x, y, size)
  local colors = {
    {80, 160, 220},
    {220, 120, 160},
    {120, 180, 100},
    {180, 140, 80},
    {140, 100, 200},
  }
  local idx = 1
  if name and #name > 0 then
    idx = (string.byte(name, 1) % #colors) + 1
  end
  local c = colors[idx]
  local r = math.floor(size / 2)
  watch.circle(x + r, y + r, r, c[1], c[2], c[3])
  local initial = "?"
  if name and #name > 0 then
    initial = string.upper(string.sub(name, 1, 1))
  end
  -- Approximate center for 16px font
  watch.text_at(x + r - 5, y + r - 8, initial, 250, 250, 255)
end


local function prep_sd()
  if not watch.sd_mount or not watch.sd_mkdir or not watch.sd_put_lfs or not watch.lfs_list then
    watch.set_text("Prep SD: bindings missing\n(need sd_mkdir/sd_put_lfs/lfs_list)")
    return
  end
  if not watch.sd_mount() then
    watch.set_text("Prep SD: mount failed")
    return
  end
  watch.sd_mkdir("/sd/squish")
  watch.sd_mkdir("/sd/squish/img")
  local entries = watch.lfs_list("/apps/squish_id/img")
  if not entries then
    watch.set_text("Prep SD: LFS list failed")
    return
  end
  local total = 0
  local copied = 0
  local fails = 0
  local first_fail = nil
  for i = 1, #entries do
    local e = entries[i]
    local name = e.name or ""
    if not e.is_dir and string.sub(name, -5) == ".wrgb" then
      total = total + 1
      local src = "/apps/squish_id/img/" .. name
      local dst = "/sd/squish/img/" .. name
      -- Do not wipe existing SD files; overwrite same name is OK.
      local ok = watch.sd_put_lfs(src, dst)
      if ok then
        copied = copied + 1
      else
        fails = fails + 1
        if not first_fail then first_fail = name end
      end
    end
  end
  local msg = string.format("Prep SD\ncopied %d / total %d", copied, total)
  if fails > 0 then
    msg = msg .. string.format("\nfail %d e.g. %s", fails, first_fail or "?")
  else
    msg = msg .. "\n(no wipe; existing kept)"
  end
  watch.set_text(msg)
  print(msg)
end


local build_search  -- forward decl
local build_details

local function show_detail_for(r)
  if not r then return end
  detail = r
  mode = "details"
  build_details()
end

build_details = function()
  watch.clear_ui()
  if watch.image_clear then watch.image_clear() end
  watch.button_layout("column")
  watch.set_title("Details")
  watch.fill(8, 10, 16)

  local r = detail
  if not r then
    watch.set_text("No entry")
    watch.button("Back", function()
      mode = "search"
      build_search()
    end)
    return
  end

  local img_size = 96
  local img_x = math.floor((watch.width() - img_size) / 2)
  local img_y = inset + 36
  local shown = false
  local path = nil
  if watch.squish_image_path then
    path = watch.squish_image_path(r.name or "")
  end
  if path and watch.image then
    shown = watch.image(path, img_x, img_y, img_size, img_size)
  end
  if not shown then
    placeholder(r.name or "?", img_x, img_y, img_size)
  end

  local title = r.full_name
  if not title or title == "" then title = r.name or "?" end
  local lines = {}
  table.insert(lines, "")
  table.insert(lines, "")
  table.insert(lines, "")
  table.insert(lines, "")
  table.insert(lines, "")
  table.insert(lines, trunc(title, 36))
  if r.animal and r.animal ~= "" then
    table.insert(lines, "Animal: " .. trunc(r.animal, 28))
  end
  if r.size and r.size ~= "" then
    table.insert(lines, 'Size: ' .. r.size .. '"')
  end
  table.insert(lines, "Price: " .. price_of(r))
  if r.squad and r.squad ~= "" then
    table.insert(lines, "Squad: " .. trunc(r.squad, 28))
  end
  if r.bio and r.bio ~= "" then
    table.insert(lines, "")
    table.insert(lines, trunc(r.bio, 90))
  end
  if not shown then
    table.insert(lines, "")
    table.insert(lines, "(no image)")
  end
  watch.set_text(table.concat(lines, "\n"))

  watch.button("Back", function()
    if watch.image_clear then watch.image_clear() end
    mode = "search"
    build_search()
  end)
end

local function refresh_search_text()
  local ncat = watch.squish_count()
  local lines = {}
  table.insert(lines, "Q: " .. (query ~= "" and query or "(type)"))
  table.insert(lines, "")

  if query == "" then
    table.insert(lines, string.format("Catalog: %d offline", ncat))
    table.insert(lines, "Tap letters to search")
    table.insert(lines, "Open1/2/3 = details")
    table.insert(lines, "BKSP delete - CLR clear")
  else
    results = watch.squish_search(query) or {}
    local n = #results
    if n == 0 then
      table.insert(lines, "No matches")
    else
      local pages = math.floor((n - 1) / PAGE_SIZE) + 1
      if page >= pages then page = pages - 1 end
      if page < 0 then page = 0 end
      local start = page * PAGE_SIZE + 1
      local last = math.min(start + PAGE_SIZE - 1, n)
      table.insert(lines, string.format("%d hit%s - p%d/%d", n, n == 1 and "" or "s", page + 1, pages))
      local slot = 1
      for i = start, last do
        local r = results[i]
        local title = r.full_name
        if not title or title == "" then title = r.name end
        title = trunc(title, 30)
        table.insert(lines, string.format("%d) %s  %s", slot, title, price_of(r)))
        if r.animal and r.animal ~= "" then
          local sub = r.animal
          if r.size and r.size ~= "" then sub = sub .. " - " .. r.size .. '"' end
          table.insert(lines, "   " .. trunc(sub, 34))
        end
        slot = slot + 1
      end
    end
  end

  watch.set_text(table.concat(lines, "\n"))
end

build_search = function()
  watch.clear_ui()
  if watch.image_clear then watch.image_clear() end
  watch.button_layout("grid2")
  watch.set_title("Squish ID")
  watch.fill(6, 10, 14)

  watch.letter_pad(function(key)
    if key == "BKSP" then
      if #query > 0 then
        query = string.sub(query, 1, #query - 1)
      end
    elseif key == "CLR" then
      query = ""
    elseif type(key) == "string" and #key == 1 then
      if #query < 24 then
        query = query .. key
      end
    end
    page = 0
    refresh_search_text()
  end)

  local function open_slot(slot)
    if query == "" then return end
    results = watch.squish_search(query) or {}
    local rows = page_rows()
    if slot >= 1 and slot <= #rows then
      show_detail_for(rows[slot])
    end
  end

  watch.button("Open1", function() open_slot(1) end)
  watch.button("Open2", function() open_slot(2) end)
  watch.button("Open3", function() open_slot(3) end)

  watch.button("More", function()
    local n = #results
    if n == 0 and query ~= "" then
      results = watch.squish_search(query) or {}
      n = #results
    end
    if n == 0 then return end
    local pages = math.floor((n - 1) / PAGE_SIZE) + 1
    page = (page + 1) % pages
    refresh_search_text()
  end)

  watch.button("Prep SD", function()
    prep_sd()
  end)

  watch.button("Exit", function()
    if watch.image_clear then watch.image_clear() end
    watch.back()
  end)

  refresh_search_text()
end

build_search()
print(string.format("Squish ID ready - %d in catalog", watch.squish_count()))
