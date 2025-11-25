import "CoreLibs/graphics"
import "CoreLibs/sprites"
import "CoreLibs/animation"

local gfx = playdate.graphics
local geo = playdate.geometry
local net = playdate.network

local fnt = gfx.font.new("fonts/SYSTEM6")
gfx.setFont(fnt)

-- Constants
local SCREEN_WIDTH, SCREEN_HEIGHT = playdate.display.getSize()
local CURSOR_SIZE = 25
local CURSOR_COLLISION_RECT = {x = 8, y = 8, w = 9, h = 9}
local CURSOR_ZINDEX = 32767

local PAGE_PADDING = 10

-- D-pad scrolling
local scroll = {
	animator = nil,
	easing = playdate.easingFunctions.outQuint,
	distance = 220,
	duration = 400
}

-- Navigation state
local nav = {
	history = {},
	currentURL = nil,
	pending = false,
	buffer = "",
	initialPageLoaded = false,
}

-- Forward declaration (defined after render)
local menu

-- Favorites
local favorites = {
	file = "favorites",
	items = {},  -- array of {url=, title=}
}

function favorites:load()
	self.items = playdate.datastore.read(self.file) or {}
end

function favorites:save()
	playdate.datastore.write(self.items, self.file)
end

function favorites:contains(url)
	for _, fav in ipairs(self.items) do
		if fav.url == url then return true end
	end
	return false
end

function favorites:add(url, title)
	if not self:contains(url) then
		table.insert(self.items, {url = url, title = title})
		self:save()
	end
end

function favorites:remove(url)
	for i, fav in ipairs(self.items) do
		if fav.url == url then
			table.remove(self.items, i)
			self:save()
			return
		end
	end
end

function favorites:getTitleFromURL(url)
	local host = string.match(url, "^https?://([^/]+)")
	local path = string.match(url, "^https?://[^/]+(.*)") or "/"
	if path == "/" or path == "" then
		return host or url
	end
	local segment = string.match(path, "/([^/]+)$") or path
	return string.gsub(segment, "%.%w+$", "")
end

-- Viewport
local viewport = {
	top = 0
}

-- Page initialization
function initializePage()
	local page = gfx.sprite.new()
	page:setSize(100, 100)
	page:setCenter(0, 0)
	page:moveTo(0, 0)
	page:add()

	page.height = 0
	page.width = SCREEN_WIDTH
	page.padding = PAGE_PADDING
	page.contentWidth = page.width - 2 * page.padding
	page.links = {}  -- Array of Link objects

	return page
end

local page = initializePage()

function viewport:moveTo(newTop)
	local dy = self.top - newTop  -- positive when scrolling down (sprites move up)
	self.top = newTop

	-- Move page and all links physically
	page:moveBy(0, dy)
	for _, link in ipairs(page.links) do
		link:moveBy(0, dy)
	end
end

-- Cursor initialization
function initializeCursor()
	local cursor = gfx.sprite.new()

	cursor:moveTo(130, 42)
	cursor:setSize(CURSOR_SIZE, CURSOR_SIZE)
	cursor:setZIndex(CURSOR_ZINDEX)
	cursor:setCollideRect(CURSOR_COLLISION_RECT.x, CURSOR_COLLISION_RECT.y,
	                      CURSOR_COLLISION_RECT.w, CURSOR_COLLISION_RECT.h)
	cursor:setGroups({1})
	cursor:add()

	cursor.collisionResponse = gfx.sprite.kCollisionTypeOverlap
	cursor.speed = 0
	cursor.thrust = 0.5
	cursor.maxSpeed = 8
	cursor.friction = 0.8

	cursor.blinker = gfx.animation.blinker.new(200, 200, true, 6, true)
	cursor.blinker:stop()

	return cursor
end

local cursor = initializeCursor()

function cursor:updateImage()
	local w, h = self:getSize()
	local centerX, centerY = math.floor(w / 2) + 1, math.floor(h / 2) + 1
	local moon = geo.point.new(centerX, h - 3)
	local transform = geo.affineTransform.new()

	transform:rotate(playdate.getCrankPosition(), centerX, centerY)
	transform:transformPoint(moon)

	local moonX, moonY = moon:unpack()

	-- Create new image
	local cursorImage = gfx.image.new(CURSOR_SIZE, CURSOR_SIZE, gfx.kColorClear)
	gfx.pushContext(cursorImage)

	-- Draw collision area (white, for alphaCollision)
	gfx.setColor(gfx.kColorWhite)
	gfx.fillRect(CURSOR_COLLISION_RECT.x, CURSOR_COLLISION_RECT.y,
	             CURSOR_COLLISION_RECT.w, CURSOR_COLLISION_RECT.h)

	-- Draw white outer squares (visible cursor design)
	gfx.setColor(gfx.kColorWhite)
	gfx.fillRect(moonX - 3, moonY - 3, 5, 5)
	gfx.fillRect(8, 8, 9, 9)

	-- Draw black inner squares
	gfx.setColor(gfx.kColorBlack)
	gfx.fillRect(9, 9, 7, 7)

	if not self.blinker.running or self.blinker.on then
		gfx.fillRect(moonX - 2, moonY - 2, 3, 3)
	end

	gfx.popContext()

	self:setImage(cursorImage)
end

cursor:updateImage()  -- Set initial cursor image

-- Link class that represents a clickable link (extends sprite)
-- Defined after cursor so it can access cursor as an upvalue
class('Link').extends(gfx.sprite)

function Link:init(url, segments, font, padding)
	-- Calculate bounding box from all segments
	local minX = math.huge
	local minY = math.huge
	local maxX = -math.huge
	local maxY = -math.huge

	for _, seg in ipairs(segments) do
		local w = font:getTextWidth(seg.text)
		minX = math.min(minX, seg.x)
		minY = math.min(minY, seg.y)
		maxX = math.max(maxX, seg.x + w)
		maxY = math.max(maxY, seg.y)
	end

	local textHeight = font:getHeight()
	local width = maxX - minX
	local height = maxY - minY + textHeight

	if width <= 0 or height <= 0 or width ~= width then
		error("Invalid link dimensions: " .. tostring(width) .. "x" .. tostring(height))
	end

	Link.super.init(self)

	self.url = url
	self.segments = segments  -- Array of {x, y, text}
	self.font = font
	self.textHeight = textHeight
	self.offsetX = minX
	self.offsetY = minY

	local image = gfx.image.new(width, height)
	if not image then
		error("Failed to create link image")
	end

	self:drawSegments(image, false)
	self:setImage(image)
	self:setSize(width, height)
	self:setCenter(0, 0)
	self:moveTo(padding + minX, padding + minY)
	self:setZIndex(1)  -- Above page content (link draws its own text)
	self:setCollideRect(0, 0, width, height)
	self:setCollidesWithGroups({1})
	self.collisionResponse = gfx.sprite.kCollisionTypeOverlap
	self.wasHovered = false
end

function Link:update()
	local isHovered = cursor:alphaCollision(self)
	if isHovered ~= self.wasHovered then
		self.wasHovered = isHovered
		self:redrawImage(isHovered)
	end
end

function Link:drawSegments(image, isHovered)
	gfx.pushContext(image)

	for _, seg in ipairs(self.segments) do
		local localX = seg.x - self.offsetX
		local localY = seg.y - self.offsetY
		local w = self.font:getTextWidth(seg.text)

		-- Draw white background (for collision detection)
		gfx.setColor(gfx.kColorWhite)
		gfx.fillRect(localX, localY, w, self.textHeight)

		-- Draw text
		self.font:drawText(seg.text, localX, localY)

		-- Draw underline (thick if hovered)
		gfx.setColor(gfx.kColorBlack)
		if isHovered then
			gfx.setLineWidth(2)
		end
		gfx.drawLine(localX, localY + self.textHeight - 2,
		             localX + w, localY + self.textHeight - 2)
		if isHovered then
			gfx.setLineWidth(1)
		end
	end

	gfx.popContext()
end

function Link:redrawImage(isHovered)
	local width, height = self:getSize()
	local image = gfx.image.new(width, height, gfx.kColorClear)
	self:drawSegments(image, isHovered)
	self:setImage(image)
end

function parseURL(url)
	local secure = string.match(url, "^https://") ~= nil
	local host = string.match(url, "^https?://([^/]+)")
	local path = string.match(url, "^https?://[^/]+(.*)") or "/"
	local port = secure and 443 or 80
	return host, port, secure, path
end

-- url = nil means go back in history
function fetchPage(url)
	if nav.pending then return end

	if url then
		if nav.currentURL then
			table.insert(nav.history, nav.currentURL)
		end
	else
		url = table.remove(nav.history)
		if not url then return end
	end

	nav.pending = true
	nav.buffer = ""
	cursor.blinker:start()

	local host, port, secure, path = parseURL(url)
	local conn = net.http.new(host, port, secure, "ORBIT")
	if not conn then
		nav.pending = false
		return
	end

	conn:setConnectTimeout(10)

	conn:setRequestCallback(function()
		local bytes = conn:getBytesAvailable()
		if bytes > 0 then
			local chunk = conn:read(bytes)
			if chunk then
				nav.buffer = nav.buffer .. chunk
			end
		end
	end)

	conn:setRequestCompleteCallback(function()
		local err = conn:getError()
		if err and err ~= "Connection closed" then
			nav.pending = false
			return
		end

		local success = pcall(render, nav.buffer)
		if not success then
			nav.pending = false
			return
		end

		nav.currentURL = url
		menu:updateCheckmark()
		nav.pending = false
		cursor.blinker:stop()
		cursor:updateImage()
	end)

	conn:get(path)
end

function page:cleanupLinks()
	for _, link in ipairs(self.links) do
		if link then link:remove() end
	end
	self.links = {}
end

-- Word-wrapping layout function
-- Returns: newX, newY, segments array of {x, y, text}
local function layoutWords(text, x, y, font, contentWidth)
	local segments = {}
	local h = font:getHeight()
	local sw = font:getTextWidth(" ")
	local pos = 1

	while pos <= #text do
		if string.sub(text, pos, pos) == " " then
			if x + sw <= contentWidth then
				x = x + sw
			end
			pos = pos + 1
		else
			local wordEnd = string.find(text, " ", pos) or #text + 1
			local word = string.sub(text, pos, wordEnd - 1)
			local w = font:getTextWidth(word)

			-- Wrap if needed
			if x > 0 and x + w > contentWidth then
				y = y + h
				x = 0
			end

			table.insert(segments, {x = x, y = y, text = word})
			x = x + w
			pos = wordEnd
		end
	end

	return x, y, segments
end

function render(text)
	page:cleanupLinks()
	viewport.top = 0

	local fragments = json.decode(cmark.parse(text)) or {}
	local h = fnt:getHeight()
	local textSegments = {}

	-- Layout pass: create links, buffer text segments
	local x, y = 0, 0
	for _, frag in ipairs(fragments) do
		if frag.type == "break" then
			x, y = 0, y + h * 2
		else
			local segments
			x, y, segments = layoutWords(frag.text, x, y, fnt, page.contentWidth)
			if frag.type == "link" then
				local success, link = pcall(Link, frag.url, segments, fnt, page.padding)
				if success then
					link:add()
					table.insert(page.links, link)
				end
			else
				for _, seg in ipairs(segments) do
					table.insert(textSegments, seg)
				end
			end
		end
	end

	-- Create image and draw text
	page.height = math.max(SCREEN_HEIGHT, y + h + 2 * page.padding)
	local pageImage = gfx.image.new(page.width, page.height)
	if not pageImage then return end

	gfx.pushContext(pageImage)
	for _, seg in ipairs(textSegments) do
		fnt:drawText(seg.text, page.padding + seg.x, page.padding + seg.y)
	end
	gfx.popContext()

	page:setImage(pageImage)
	page:moveTo(0, 0)
end

-- Menu setup
menu = {
	handle = playdate.getSystemMenu(),
	checkmark = nil,
	options = nil,
}

function menu:updateCheckmark()
	if self.checkmark then
		self.checkmark:setValue(nav.currentURL and favorites:contains(nav.currentURL))
	end
end

function menu:updateOptions()
	if self.options then
		self.handle:removeMenuItem(self.options)
		self.options = nil
	end

	if #favorites.items >= 2 then
		local titles = {}
		for _, fav in ipairs(favorites.items) do
			table.insert(titles, fav.title)
		end
		self.options = self.handle:addOptionsMenuItem("open", titles, "tutorial", function(title)
			for _, fav in ipairs(favorites.items) do
				if fav.title == title then
					fetchPage(fav.url)
					break
				end
			end
		end)
	end
end

function menu:init()
	self.checkmark = self.handle:addCheckmarkMenuItem("Save", false, function(checked)
		if nav.currentURL then
			if checked then
				favorites:add(nav.currentURL, favorites:getTitleFromURL(nav.currentURL))
			else
				favorites:remove(nav.currentURL)
			end
			self:updateOptions()
		end
	end)

	favorites:load()
	if #favorites.items < 2 then
		favorites.items = {
			{url = "https://orbit.casa/tutorial.md", title = "tutorial"},
			{url = "https://orbit.casa/boo.md", title = "boo"},
		}
		favorites:save()
	end
	self:updateOptions()
end

menu:init()

local function handleNavInput()
	-- A/RIGHT to activate links
	if playdate.buttonJustPressed(playdate.kButtonRight) or
	   playdate.buttonJustPressed(playdate.kButtonA) then
		for _, link in ipairs(cursor:overlappingSprites()) do
			if cursor:alphaCollision(link) then
				fetchPage(link.url)
				break
			end
		end
	end

	-- B to go back
	if playdate.buttonJustPressed(playdate.kButtonB) then
		fetchPage(nil)
	end
end

local function updateCursor()
	-- Update image if crank moved or blinking
	if playdate.getCrankChange() ~= 0 or cursor.blinker.running then
		cursor:updateImage()
	end

	-- Thrust with UP button
	if playdate.buttonIsPressed(playdate.kButtonUp) then
		cursor.speed = math.min(cursor.maxSpeed, cursor.speed + cursor.thrust)
	else
		cursor.speed = cursor.speed * cursor.friction
	end

	if cursor.speed == 0 then return end

	local radians = math.rad(playdate.getCrankPosition() - 90)
	local vx = math.cos(radians) * cursor.speed
	local vy = math.sin(radians) * cursor.speed

	local screenX, screenY = cursor:getPosition()
	local targetX = (screenX + vx) % SCREEN_WIDTH
	local targetY = screenY + vy

	-- Auto-scroll at screen edges
	if targetY <= 0 and viewport.top > 0 then
		viewport:moveTo(viewport.top - math.min(viewport.top, -targetY))
	elseif targetY >= SCREEN_HEIGHT and viewport.top + SCREEN_HEIGHT < page.height then
		local maxScroll = page.height - SCREEN_HEIGHT - viewport.top
		viewport:moveTo(viewport.top + math.min(maxScroll, targetY - SCREEN_HEIGHT))
	end

	-- Move cursor (clamped to screen)
	local clampedY = math.max(0, math.min(SCREEN_HEIGHT, targetY))
	local _, _, collisions = cursor:moveWithCollisions(targetX, clampedY)
	for _, c in ipairs(collisions) do
		c.other:markDirty()
	end
end

local function updateScroll()
	-- D-pad scrolling
	if playdate.buttonJustPressed(playdate.kButtonDown) then
		local maxTop = math.max(page.height - SCREEN_HEIGHT, 0)
		scroll.animator = gfx.animator.new(scroll.duration, viewport.top,
			math.min(maxTop, viewport.top + scroll.distance), scroll.easing)
	elseif playdate.buttonJustPressed(playdate.kButtonLeft) then
		scroll.animator = gfx.animator.new(scroll.duration, viewport.top,
			math.max(0, viewport.top - scroll.distance), scroll.easing)
	end

	if scroll.animator then
		viewport:moveTo(scroll.animator:currentValue())
		if scroll.animator:ended() then
			scroll.animator = nil
		end
	end
end

function playdate.update()
	if not nav.initialPageLoaded then
		fetchPage("https://orbit.casa/tutorial.md")
		nav.initialPageLoaded = true
	end

	handleNavInput()
	updateCursor()
	updateScroll()

	gfx.sprite.update()
	gfx.animation.blinker.updateAll()
end
