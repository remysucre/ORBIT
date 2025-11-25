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
local SCREEN_CENTER_X = SCREEN_WIDTH / 2
local SCREEN_CENTER_Y = SCREEN_HEIGHT / 2

local CURSOR_SIZE = 25
local CURSOR_COLLISION_RECT = {x = 8, y = 8, w = 9, h = 9}
local CURSOR_ZINDEX = 32767

local MESSAGE_RECT = {x = 20, y = 100, w = 360, h = 140}

local PAGE_PADDING = 10

-- D-pad scrolling
local scroll = {
	animator = nil,
	easing = playdate.easingFunctions.outQuint,
	distance = 220,
	duration = 400
}

-- History stack for back navigation
local history = {}
local currentURL = nil

-- HTTP request data buffer
local httpData = nil

-- Favorites
local favoritesFile = "favorites"
local favorites = {} -- table of {url=, title=}

function loadFavorites()
	local data = playdate.datastore.read(favoritesFile)
	if data then
		favorites = data
	else
		favorites = {}
	end
end

function saveFavorites()
	playdate.datastore.write(favorites, favoritesFile)
end

function isFavorited(url)
	for _, fav in ipairs(favorites) do
		if fav.url == url then
			return true
		end
	end
	return false
end

function addFavorite(url, title)
	if not isFavorited(url) then
		table.insert(favorites, {url = url, title = title})
		saveFavorites()
	end
end

function removeFavorite(url)
	for i, fav in ipairs(favorites) do
		if fav.url == url then
			table.remove(favorites, i)
			saveFavorites()
			return
		end
	end
end

function getTitleFromURL(url)
	-- Extract a simple title from URL
	local host = string.match(url, "^https?://([^/]+)")
	local path = string.match(url, "^https?://[^/]+(.*)") or "/"
	if path == "/" or path == "" then
		return host or url
	end
	-- Use last path segment as title
	local segment = string.match(path, "/([^/]+)$") or path
	-- Remove file extension
	segment = string.gsub(segment, "%.%w+$", "")
	return segment
end

-- -- Utility: Display a message on screen
-- function showMessage(message)
-- 	-- playdate.stop()
-- 	-- gfx.clear()
-- 	-- gfx.setDrawOffset(0, 0)
-- 	local img = gfx.imageWithText(message, MESSAGE_RECT.w, MESSAGE_RECT.h,
-- 	                                  gfx.kTextAlignment.center)
-- 	img:drawIgnoringOffset(MESSAGE_RECT.x, MESSAGE_RECT.y)
-- 	-- playdate.display.flush()
-- end


-- Viewport
local viewport = {
	top = 0
}

-- Page initialization
function initializePage()
	local page = gfx.sprite.new()
	page:setSize(100, 100)
	page:moveTo(SCREEN_CENTER_X, SCREEN_CENTER_Y)
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
	-- Calculate bounding box from all segments first
	local minX = math.huge
	local minY = math.huge
	local maxX = -math.huge
	local maxY = -math.huge

	for _, seg in ipairs(segments) do
		minX = math.min(minX, seg.x)
		minY = math.min(minY, seg.y)
		maxX = math.max(maxX, seg.x + seg.width)
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
	self.segments = segments  -- Array of {x, y, width}
	self.font = font
	self.textHeight = textHeight

	-- Store offset for drawing
	self.offsetX = minX
	self.offsetY = minY

	-- Create sprite image with collision boxes and underlines
	local image = gfx.image.new(width, height)
	if not image then
		error("Failed to create link image")
	end

	gfx.pushContext(image)

	for _, seg in ipairs(segments) do
		local localX = seg.x - minX
		local localY = seg.y - minY

		-- Draw white collision box
		gfx.setColor(gfx.kColorWhite)
		gfx.fillRect(localX, localY, seg.width, self.textHeight)

		-- Draw black underline
		gfx.setColor(gfx.kColorBlack)
		gfx.drawLine(localX, localY + self.textHeight - 2,
		             localX + seg.width, localY + self.textHeight - 2)
	end

	gfx.popContext()

	self:setImage(image)
	self:setSize(width, height)
	self:setCenter(0, 0)
	self:moveTo(padding + minX, padding + minY)
	self:setZIndex(-1)  -- Behind page content
	self:setCollideRect(0, 0, width, height)
	self:setCollidesWithGroups({1})
	self.collisionResponse = gfx.sprite.kCollisionTypeOverlap
	self.wasHovered = false
end

function Link:update()
	-- Only called when sprite is dirty (e.g., after collision detection)
	local isHovered = cursor:alphaCollision(self)

	-- Only redraw if hover state changed
	if isHovered ~= self.wasHovered then
		self.wasHovered = isHovered
		self:redrawImage(isHovered)
	end
end

function Link:redrawImage(isHovered)
	local width = self:getSize()
	local height = select(2, self:getSize())

	local image = gfx.image.new(width, height, gfx.kColorClear)
	gfx.pushContext(image)

	for _, seg in ipairs(self.segments) do
		local localX = seg.x - self.offsetX
		local localY = seg.y - self.offsetY

		-- Draw white collision box
		gfx.setColor(gfx.kColorWhite)
		gfx.fillRect(localX, localY, seg.width, self.textHeight)

		-- Draw underline (thick if hovered, thin otherwise)
		gfx.setColor(gfx.kColorBlack)
		if isHovered then
			gfx.setLineWidth(2)
		end
		gfx.drawLine(localX, localY + self.textHeight - 2,
		             localX + seg.width, localY + self.textHeight - 2)
		if isHovered then
			gfx.setLineWidth(1)
		end
	end

	gfx.popContext()
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

	if httpData then
		-- A request is already in progress
		return
	end

	if url then
		if currentURL then
			table.insert(history, currentURL)
		end
	else -- Go back in history
		url = table.remove(history)
		if not url then
			-- No history to go back to
			return
		end
	end

	-- Show loading message and set loading state
	-- showMessage("Loading...")
	httpData = ""

	-- Start cursor blinking to indicate loading
	cursor.blinker:start()

	-- Parse URL and create HTTP connection
	local host, port, secure, path = parseURL(url)
	local httpConn = net.http.new(host, port, secure, "ORBIT")

	if not httpConn then
		-- showMessage("Error: Network access denied")
		httpData = nil
		return
	end

	httpConn:setConnectTimeout(10)

	-- Callback to receive data chunks
	httpConn:setRequestCallback(function()
		local bytes = httpConn:getBytesAvailable()
		if bytes > 0 then
			local chunk = httpConn:read(bytes)
			if chunk then
				httpData = httpData .. chunk
			end
		end
	end)

	-- Callback when request completes
	httpConn:setRequestCompleteCallback(function()

		-- Check for errors
		local err = httpConn:getError()
		if err and err ~= "Connection closed" then
			-- showMessage("Error: " .. err)
			httpData = nil
			return
		end

		-- Render the page
		local success, renderErr = pcall(render, httpData)
		if not success then
			-- showMessage("Failed to render page: " .. tostring(renderErr))
			httpData = nil
			return
		end

		-- Update current URL after successful render
		currentURL = url
		updateFavoriteCheckmark()

		-- Clear loading state
		httpData = nil
		cursor.blinker:stop()
		cursor.on = true
		cursor:updateImage()
	end)

	-- Start the request
	httpConn:get(path)
end

function cleanupLinks()
	for _, link in ipairs(page.links) do
		if link then
			link:remove()
		end
	end
	page.links = {}
end

-- Shared word-wrapping function for both plain text and links
-- Returns: newX, newY, draws array, segments array (if trackSegments)
local function layoutWords(text, x, y, font, contentWidth, trackSegments)
	local draws = {}
	local segments = trackSegments and {} or nil
	local h = font:getHeight()
	local segStartX, segStartY = x, y

	for word in string.gmatch(text, "%S+") do
		local w = font:getTextWidth(word)
		local sw = font:getTextWidth(" ")

		if x > 0 and x + w > contentWidth then
			-- Save segment before wrap (if tracking)
			if segments and x > segStartX then
				table.insert(segments, {x = segStartX, y = segStartY, width = x - segStartX})
			end
			y = y + h
			x = 0
			segStartX, segStartY = x, y
		end

		table.insert(draws, {txt = word, x = x, y = y})
		x = x + w

		if x + sw <= contentWidth then
			table.insert(draws, {txt = " ", x = x, y = y})
			x = x + sw
		end
	end

	-- Final segment (trim trailing space)
	if segments and x > segStartX then
		local trailingSpace = font:getTextWidth(" ")
		local segWidth = x - segStartX - trailingSpace
		if segWidth > 0 then
			table.insert(segments, {x = segStartX, y = segStartY, width = segWidth})
		end
	end

	return x, y, draws, segments
end

-- Layout fragments from cmark parser
-- fragments: array of {type="text"|"link", text=string, url=string (for links)}
-- Returns: toDraw commands, contentHeight, and populates page.links
function layoutFragments(fragments)
	local h = fnt:getHeight()
	local x, y = 0, 0
	local toDraw = {}
	for _, frag in ipairs(fragments) do
		local text = frag.text or ""

		if frag.type == "link" then
			-- Link fragment - use shared layout with segment tracking
			local draws, segments
			x, y, draws, segments = layoutWords(text, x, y, fnt, page.contentWidth, true)

			for _, d in ipairs(draws) do
				table.insert(toDraw, d)
			end

			-- Create link sprite if we have segments
			if segments and #segments > 0 then
				local success, link = pcall(function()
					return Link(frag.url, segments, fnt, page.padding)
				end)

				if success then
					link:add()
					table.insert(page.links, link)
				else
					print("Failed to create link:", text, "error:", link)
				end
			end

		else
			-- Text fragment - handle newlines and word wrapping
			for line in string.gmatch(text .. "\n", "([^\n]*)\n") do
				if line ~= "" then
					local draws
					x, y, draws = layoutWords(line, x, y, fnt, page.contentWidth, false)
					for _, d in ipairs(draws) do
						table.insert(toDraw, d)
					end
				end
				-- Move to next line after each \n
				x = 0
				y = y + h
			end
		end
	end

	local contentHeight = y + h
	return toDraw, contentHeight
end

function renderPageImage(toDraw, pageHeight)
	local pageImage = gfx.image.new(page.width, pageHeight)
	if not pageImage then
		return nil
	end

	gfx.pushContext(pageImage)
	for _, cmd in ipairs(toDraw) do
		if cmd and cmd.txt then
			fnt:drawText(cmd.txt, page.padding + cmd.x, page.padding + cmd.y)
		end
	end
	gfx.popContext()

	return pageImage
end

function render(text)
	cleanupLinks()

	-- Reset viewport to top (no sprites to move since links were just cleaned up)
	viewport.top = 0

	-- Parse markdown using cmark C extension
	local jsonStr = cmark.parse(text)

	-- Decode JSON into fragments array
	local fragments = json.decode(jsonStr) or {}

	-- Layout fragments (this also creates link sprites)
	local toDraw, contentHeight = layoutFragments(fragments)

	-- Calculate page height and render to image
	page.height = math.max(SCREEN_HEIGHT, contentHeight + 2 * page.padding)
	local pageImage = renderPageImage(toDraw, page.height)

	if pageImage then
		page:setImage(pageImage)
		page:moveTo(SCREEN_CENTER_X, page.height / 2)
	end
end

-- load homepage when app starts
local initialPageLoaded = false

-- setup menu
local menu = playdate.getSystemMenu()
local favoriteCheckmark = nil
local favoritesOptions = nil

function updateFavoriteCheckmark()
	local isChecked = currentURL and isFavorited(currentURL)
	if favoriteCheckmark then
		favoriteCheckmark:setValue(isChecked)
	end
end

function updateFavoritesOptions()
	-- Remove old options menu item if it exists
	if favoritesOptions then
		menu:removeMenuItem(favoritesOptions)
		favoritesOptions = nil
	end

	-- Only add options menu if there are at least 2 favorites
	if #favorites >= 2 then
		local titles = {}
		for _, fav in ipairs(favorites) do
			table.insert(titles, fav.title)
		end

		favoritesOptions = menu:addOptionsMenuItem("open", titles, "tutorial", function(selectedTitle)
			-- Find the URL for the selected title
			for _, fav in ipairs(favorites) do
				if fav.title == selectedTitle then
					fetchPage(fav.url)
					break
				end
			end
		end)
	end
end

favoriteCheckmark = menu:addCheckmarkMenuItem("Save", false, function(checked)
	if currentURL then
		if checked then
			local title = getTitleFromURL(currentURL)
			addFavorite(currentURL, title)
		else
			removeFavorite(currentURL)
		end
		updateFavoritesOptions()
	end
end)

-- Load favorites on startup
loadFavorites()

-- Ensure we have at least 2 favorites for the options menu
if #favorites < 2 then
	-- Clear and add defaults
	favorites = {}
	table.insert(favorites, {url = "https://orbit.casa/tutorial.md", title = "tutorial"})
	table.insert(favorites, {url = "https://orbit.casa/boo.md", title = "boo"})
	saveFavorites()
end

updateFavoritesOptions()

function playdate.update()

	if not initialPageLoaded then
		fetchPage("https://orbit.casa/tutorial.md")
		initialPageLoaded = true
	end

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

	-- B to go back in history
	if playdate.buttonJustPressed(playdate.kButtonB) then
		fetchPage(nil)
	
	end

	-- Rotate cursor if crank moves
	if playdate.getCrankChange() ~= 0 or 
	   cursor.blinker.running then
		cursor:updateImage()
	end

	-- UP to thrust cursor forward
	if playdate.buttonIsPressed(playdate.kButtonUp) then
		cursor.speed = math.min(cursor.maxSpeed, cursor.speed + cursor.thrust)
	else
		cursor.speed = cursor.speed * cursor.friction
	end

	if cursor.speed ~= 0 then
		local radians = math.rad(playdate.getCrankPosition() - 90)  -- Adjust for 0° being up
		local vx = math.cos(radians) * cursor.speed
		local vy = math.sin(radians) * cursor.speed

		local screenX, screenY = cursor:getPosition()
		local targetScreenX = (screenX + vx) % SCREEN_WIDTH
		local targetScreenY = screenY + vy

		-- Scroll page when cursor at screen edge and more content exists
		if targetScreenY <= 0 and viewport.top > 0 then
			local scrollAmount = math.min(viewport.top, -targetScreenY)
			viewport:moveTo(viewport.top - scrollAmount)
		elseif targetScreenY >= SCREEN_HEIGHT and viewport.top + SCREEN_HEIGHT < page.height then
			local scrollAmount = math.min(page.height - SCREEN_HEIGHT - viewport.top, targetScreenY - SCREEN_HEIGHT)
			viewport:moveTo(viewport.top + scrollAmount)
		end

		-- Clamp cursor to screen bounds
		local clampedScreenY = math.max(0, math.min(SCREEN_HEIGHT, targetScreenY))

		local _, _, collisions, _ = cursor:moveWithCollisions(targetScreenX, clampedScreenY)
		for _, collision in ipairs(collisions) do
			collision.other:markDirty()
		end
	end

	-- Scrolling page with D pad
	if playdate.buttonJustPressed(playdate.kButtonDown) then
		local maxTop = math.max(page.height - SCREEN_HEIGHT, 0)
		local targetTop = math.min(maxTop, viewport.top + scroll.distance)

		scroll.animator = gfx.animator.new(scroll.duration, viewport.top, targetTop, scroll.easing)
	elseif playdate.buttonJustPressed(playdate.kButtonLeft) then
		local targetTop = math.max(0, viewport.top - scroll.distance)

		scroll.animator = gfx.animator.new(scroll.duration, viewport.top, targetTop, scroll.easing)
	end

	if scroll.animator then
		viewport:moveTo(scroll.animator:currentValue())
		if scroll.animator:ended() then
			scroll.animator = nil
		end
	end

	gfx.sprite.update()
	gfx.animation.blinker.updateAll()
end
