import "CoreLibs/graphics"
import "CoreLibs/sprites"

local gfx = playdate.graphics
local geo = playdate.geometry
local net = playdate.network

local fnt = gfx.font.new("fonts/SYSTEM6")
gfx.setFont(fnt)

-- Constants
local SCREEN_WIDTH = 400
local SCREEN_HEIGHT = 240
local SCREEN_CENTER_X = 200
local SCREEN_CENTER_Y = 120

local CURSOR_SIZE = 25
local CURSOR_COLLISION_RECT = {x = 7, y = 7, w = 11, h = 11}
local CURSOR_ZINDEX = 32767

local DOGEAR_SIZE = 20
local DOGEAR_ZINDEX = 32766
local DOGEAR_POS = {x = 390, y = 10}

local MESSAGE_RECT = {x = 20, y = 100, w = 360, h = 140}

local PAGE_PADDING = 10

-- d pad scrolling
local scroll = {
	animator = nil,
	easing = playdate.easingFunctions.outQuint,
	distance = 220,
	duration = 400
}

-- history stack for back navigation
local history = {}
local currentURL = nil

-- favorites
local favoritesFile = "favorites.md"
local favorites = {} -- table of {url=, title=}

function loadFavorites()
	local file = playdate.file.open(favoritesFile, playdate.file.kFileRead)
	if not file then
		return
	end

	local content = file:read(playdate.file.getSize(favoritesFile))
	file:close()

	if not content then
		return
	end

	-- Parse md0 format to extract favorites
	favorites = {}
	local currentTitle = nil
	local currentURL = nil

	for line in string.gmatch(content .. "\n", "([^\n]*)\n") do
		-- Check for link reference [n]: url
		local num, url = string.match(line, "^%[(%d+)%]:%s+(.+)$")
		if num and url then
			local n = tonumber(num)
			if favorites[n] then
				favorites[n].url = url
			end
		else
			-- Check for link text [title][n]
			local title, num = string.match(line, "^%[([^%]]+)%]%[(%d+)%]$")
			if title and num then
				local n = tonumber(num)
				favorites[n] = {title = title, url = nil}
			end
		end
	end

	-- Convert to array and filter out incomplete entries
	local filtered = {}
	for _, fav in pairs(favorites) do
		if fav.title and fav.url then
			table.insert(filtered, fav)
		end
	end
	favorites = filtered
end

function saveFavorites()
	local file = playdate.file.open(favoritesFile, playdate.file.kFileWrite)
	if not file then
		return
	end

	local content = ""
	for i, fav in ipairs(favorites) do
		content = content .. "[" .. fav.title .. "][" .. i .. "]\n"
	end
	content = content .. "\n"
	for i, fav in ipairs(favorites) do
		content = content .. "[" .. i .. "]: " .. fav.url .. "\n"
	end

	file:write(content)
	file:close()
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

-- Utility: Display a message on screen
function showMessage(message, flush)
	gfx.clear()
	gfx.setDrawOffset(0, 0)
	gfx.drawTextInRect(message, MESSAGE_RECT.x, MESSAGE_RECT.y, MESSAGE_RECT.w, MESSAGE_RECT.h)
	if flush then
		playdate.display.flush()
	end
end

function openFavorites()
	-- Read favorites file and render it locally
	local file = playdate.file.open(favoritesFile, playdate.file.kFileRead)
	if not file then
		-- Create empty favorites file
		saveFavorites()
		file = playdate.file.open(favoritesFile, playdate.file.kFileRead)
	end

	if file then
		local content = file:read(playdate.file.getSize(favoritesFile)) or ""
		file:close()

		-- Push current URL to history before showing favorites
		if currentURL then
			table.insert(history, currentURL)
		end
		currentURL = nil -- favorites is a local page
		updateDogEar()

		render(content)
	end
end

-- viewport
local viewport = {
	top = 0
}

function viewport:moveTo(top)
	self.top = top
	gfx.setDrawOffset(0, -self.top)
end

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
	cursor.friction = 0.85

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
	gfx.fillRect(moonX - 2, moonY - 2, 3, 3)
	gfx.fillRect(9, 9, 7, 7)

	gfx.popContext()

	self:setImage(cursorImage)
end

cursor:updateImage()  -- Set initial cursor image

-- Link class that represents a clickable link (extends sprite)
-- Defined after cursor so it can access cursor as an upvalue
class('Link').extends(gfx.sprite)

function Link:init(text, url, linkNum, segments, font, padding)
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

	print("Link init:", text, "bounds:", minX, minY, maxX, maxY, "size:", width, height)

	if width <= 0 or height <= 0 or width ~= width then
		print("ERROR: Invalid dimensions!")
		error("Invalid link dimensions: " .. tostring(width) .. "x" .. tostring(height))
	end

	Link.super.init(self)

	self.text = text
	self.url = url
	self.linkNum = linkNum
	self.segments = segments  -- Array of {x, y, width}
	self.font = font
	self.textHeight = textHeight

	-- Store offset for drawing
	self.offsetX = minX
	self.offsetY = minY

	-- Create sprite image with collision boxes and underlines
	local image = gfx.image.new(width, height)
	print("Link image created:", image ~= nil)

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
	local posX = padding + minX
	local posY = padding + minY
	print("Link position:", posX, posY)
	self:moveTo(posX, posY)
	self:setZIndex(-1)  -- Behind page content
	self:setCollideRect(0, 0, width, height)
	self:setCollidesWithGroups({1})
	self.collisionResponse = gfx.sprite.kCollisionTypeOverlap
	self.wasHovered = false  -- Track hover state
	print("Link init complete")
end

function Link:updateHoverState()
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

-- dog ear indicator for favorites
local dogEar = gfx.sprite.new()
dogEar:setSize(DOGEAR_SIZE, DOGEAR_SIZE)
dogEar:setZIndex(DOGEAR_ZINDEX)
dogEar:moveTo(DOGEAR_POS.x, DOGEAR_POS.y)
dogEar:setIgnoresDrawOffset(true)
dogEar.visible = false

function dogEar:draw(x, y, width, height)
	if self.visible then
		gfx.setColor(gfx.kColorBlack)
		gfx.fillTriangle(0, 0, DOGEAR_SIZE, 0, DOGEAR_SIZE, DOGEAR_SIZE)
	end
end

dogEar:add()

function updateDogEar()
	local shouldShow = currentURL and isFavorited(currentURL)
	if dogEar.visible ~= shouldShow then
		dogEar.visible = shouldShow
		dogEar:markDirty()
	end
end

function parseURL(url)
	local secure = string.match(url, "^https://") ~= nil
	local host = string.match(url, "^https?://([^/]+)")
	local path = string.match(url, "^https?://[^/]+(.*)") or "/"
	local port = secure and 443 or 80
	return host, port, secure, path
end

function makeHttpRequest(url)
	local host, port, secure, path = parseURL(url)

	local httpConn = net.http.new(host, port, secure, "ORBIT")

	if not httpConn then
		return nil, "Network access denied"
	end

	httpConn:setConnectTimeout(10)

	local httpData = ""
	local requestComplete = false

	httpConn:setRequestCallback(function()
		local bytes = httpConn:getBytesAvailable()
		if bytes > 0 then
			local chunk = httpConn:read(bytes)
			if chunk then
				httpData = httpData .. chunk
			end
		end
	end)

	httpConn:setRequestCompleteCallback(function()
		requestComplete = true
	end)

	httpConn:get(path)

	-- Wait for request to complete
	while not requestComplete do
		playdate.wait(100)
	end

	-- Check for errors
	local err = httpConn:getError()
	if err and err ~= "Connection closed" then
		return nil, err
	end

	return httpData, nil
end

function fetchPage(url, isBack)
	-- Push current URL to history before navigating (unless going back)
	if not isBack and currentURL then
		table.insert(history, currentURL)
	end

	showMessage("Loading...", true)

	local data, err = makeHttpRequest(url)
	if err then
		showMessage("Error: " .. err)
		return
	end

	local success, renderErr = pcall(render, data)
	if not success then
		showMessage("Failed to render page: " .. tostring(renderErr))
		return
	end

	-- Update current URL after successful render
	currentURL = url
	updateDogEar()
end

function cleanupLinks()
	for _, link in ipairs(page.links) do
		if link then
			link:remove()
		end
	end
	page.links = {}
end

function parseMarkdownLinks(text, linkRefs)
	-- Process all lines to extract link definitions and create Link sprites
	for line in string.gmatch(text .. "\n", "([^\n]*)\n") do
		local num, url = string.match(line, "^%[(%d+)%]:%s+(.+)$")
		if num and url then
			local n = tonumber(num)
			if linkRefs[n] then
				-- Get link text and segments
				local linkText = linkRefs[n][1].text  -- All segments have same text
				local segments = {}

				-- Collect segment positions
				for _, ref in ipairs(linkRefs[n]) do
					table.insert(segments, {
						x = ref.x,
						y = ref.y,
						width = ref.width
					})
				end

				-- Create Link sprite with error handling
				local success, link = pcall(function()
					return Link(linkText, url, n, segments, fnt, page.padding)
				end)

				if success then
					link:add()
					table.insert(page.links, link)
				else
					print("Failed to create link:", linkText, "error:", link)
				end
			end
		end
	end
end

function layoutText(text, linkRefs)
	local h = fnt:getHeight()
	local x, y = 0, 0
	local toDraw = {}

	-- Link state tracking
	local inLink = false
	local linkStartX, linkStartY = nil, nil
	local linkText = ""
	local linkSegments = {}  -- Collect all segments for current link

	for line in string.gmatch(text .. "\n", "([^\n]*)\n") do
		-- Skip link definition lines
		local num, url = string.match(line, "^%[(%d+)%]:%s+(.+)$")
		if not num then
			-- Content line - process word by word
			for token in string.gmatch(line, "%S+") do
				if not inLink then
					-- Check if token starts with '[' - entering link mode
					if string.sub(token, 1, 1) == "[" then
						-- Check if it's a complete single-word link [word][num]
						local word, linkNum, trailing = string.match(token, "^%[([^%]]+)%]%[(%d+)%](.*)$")

						if word and linkNum then
							-- Single-word link (no spaces in link text)
							local x0, y0 = x, y
							local w = fnt:getTextWidth(word)

							if x + w > page.contentWidth then
								y = y + h
								x = 0
								x0, y0 = x, y
							end

							table.insert(toDraw, {txt = word, x = x, y = y})
							x = x + w

							-- Save link reference for later sprite creation
							local n = tonumber(linkNum)
							if not linkRefs[n] then
								linkRefs[n] = {}
							end
							table.insert(linkRefs[n], {
								text = word,
								x = x0,
								y = y0,
								width = w
							})

							-- Handle trailing characters
							if trailing and #trailing > 0 then
								local tw = fnt:getTextWidth(trailing)
								if x + tw > page.contentWidth then
									y = y + h
									x = 0
								end
								table.insert(toDraw, {txt = trailing, x = x, y = y})
								x = x + tw
							end
						else
							-- Multi-word link starting - extract first word without '['
							local firstWord = string.sub(token, 2)
							inLink = true
							linkStartX, linkStartY = x, y
							linkText = firstWord
							linkSegments = {}

							local w = fnt:getTextWidth(firstWord)
							if x + w > page.contentWidth then
								y = y + h
								x = 0
								linkStartX, linkStartY = x, y
							end

							table.insert(toDraw, {txt = firstWord, x = x, y = y})
							x = x + w
						end
					else
						-- Plain word
						local w = fnt:getTextWidth(token)
						if x + w > page.contentWidth then
							y = y + h
							x = 0
						end
						table.insert(toDraw, {txt = token, x = x, y = y})
						x = x + w
					end
				else
					-- Inside a multi-word link
					-- Check if this word ends the link with '][num]'
					local endWord, linkNum, trailing = string.match(token, "^(.+)%]%[(%d+)%](.*)$")

					if endWord then
						-- Link ends with this word
						-- Check if we need space and if the word will fit
						local sw = fnt:getTextWidth(" ")
						local w = fnt:getTextWidth(endWord)

						-- Check if word+space fits on current line
						if x + sw + w <= page.contentWidth then
							-- Both space and word fit
							table.insert(toDraw, {txt = " ", x = x, y = y})
							x = x + sw
						else
							-- Word doesn't fit (with or without space) - save current segment and wrap
							if x > linkStartX then
								local segWidth = x - linkStartX
								if segWidth > 0 then
									table.insert(linkSegments, {
										x = linkStartX,
										y = linkStartY,
										width = segWidth
									})
								end
							end
							y = y + h
							x = 0
							linkStartX, linkStartY = x, y
						end

						table.insert(toDraw, {txt = endWord, x = x, y = y})
						linkText = linkText .. " " .. endWord
						x = x + w

						-- Save final link segment
						local segWidth = x - linkStartX
						table.insert(linkSegments, {
							x = linkStartX,
							y = linkStartY,
							width = segWidth
						})

						-- Now add all segments to linkRefs with the complete link text
						local n = tonumber(linkNum)
						if not linkRefs[n] then
							linkRefs[n] = {}
						end
						for _, seg in ipairs(linkSegments) do
							table.insert(linkRefs[n], {
								text = linkText,
								x = seg.x,
								y = seg.y,
								width = seg.width
							})
						end

						-- Exit link mode
						inLink = false
						linkText = ""
						linkStartX, linkStartY = nil, nil
						linkSegments = {}

						-- Handle trailing characters
						if trailing and #trailing > 0 then
							local tw = fnt:getTextWidth(trailing)
							if x + tw > page.contentWidth then
								y = y + h
								x = 0
							end
							table.insert(toDraw, {txt = trailing, x = x, y = y})
							x = x + tw
						end
					else
						-- Middle word in link - continue accumulating
						local sw = fnt:getTextWidth(" ")
						local w = fnt:getTextWidth(token)

						-- Check if word+space fits on current line
						if x + sw + w <= page.contentWidth then
							-- Both space and word fit
							table.insert(toDraw, {txt = " ", x = x, y = y})
							x = x + sw
						else
							-- Word doesn't fit - save current segment and wrap
							if x > linkStartX then
								local segWidth = x - linkStartX
								if segWidth > 0 then
									table.insert(linkSegments, {
										x = linkStartX,
										y = linkStartY,
										width = segWidth
									})
								end
							end
							y = y + h
							x = 0
							linkStartX, linkStartY = x, y
						end

						table.insert(toDraw, {txt = token, x = x, y = y})
						linkText = linkText .. " " .. token
						x = x + w
					end
				end

				-- Add space after word (except when in link)
				if not inLink then
					local sw = fnt:getTextWidth(" ")
					if x + sw <= page.contentWidth then
						table.insert(toDraw, {txt = " ", x = x, y = y})
						x = x + sw
					end
				end
			end

			-- Move to next line
			x = 0
			y = y + h
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
	print("=== render() start ===")
	cleanupLinks()

	-- Stop cursor momentum
	cursor.speed = 0

	-- Preserve cursor's screen-relative position
	local cursorX, cursorY = cursor:getPosition()
	local screenY = cursorY - viewport.top

	-- Reset viewport to top
	viewport:moveTo(0)

	-- Layout text and collect link references
	local linkRefs = {}
	local toDraw, contentHeight = layoutText(text, linkRefs)
	print("Layout complete, contentHeight:", contentHeight, "toDraw items:", #toDraw)

	-- Create link sprites from references
	parseMarkdownLinks(text, linkRefs)
	print("Links parsed, count:", #page.links)

	-- Calculate page height and render to image
	page.height = math.max(SCREEN_HEIGHT, contentHeight + 2 * page.padding)
	local pageImage = renderPageImage(toDraw, page.height)
	print("Page image created:", pageImage ~= nil, "size:", page.height)

	if pageImage then
		page:setImage(pageImage)
		page:moveTo(SCREEN_CENTER_X, page.height / 2)
		print("Page image set")

		-- Set cursor to same screen position in new page
		local newY = math.min(screenY, page.height)
		cursor:moveTo(cursorX, newY)
		print("Cursor positioned at:", cursorX, newY)
	else
		print("ERROR: pageImage is nil!")
	end
	print("=== render() end ===")
end

-- load homepage when app starts
local initialPageLoaded = false

-- setup menu
local menu = playdate.getSystemMenu()

menu:addMenuItem("Favorites", function()
	openFavorites()
end)

-- Load favorites on startup
loadFavorites()

-- A button held callback for favorite toggle
function playdate.AButtonHeld()
	if currentURL then
		if isFavorited(currentURL) then
			removeFavorite(currentURL)
		else
			local title = getTitleFromURL(currentURL)
			addFavorite(currentURL, title)
		end
		updateDogEar()
	end
end

function playdate.update()

	if not initialPageLoaded then
		-- fetchPage("https://orbit.casa/tutorial.md")
		fetchPage("https://orbit.casa/test-multiword-links.md")
		initialPageLoaded = true
	end

	-- scrolling page with D pad
	if playdate.buttonJustPressed(playdate.kButtonDown) then
		local maxTop = math.max(page.height - SCREEN_HEIGHT, 0)
		local targetTop = math.min(maxTop, viewport.top + scroll.distance)

		scroll.animator = gfx.animator.new(scroll.duration, viewport.top, targetTop, scroll.easing)
	elseif playdate.buttonJustPressed(playdate.kButtonLeft) then
		local targetTop = math.max(0, viewport.top - scroll.distance)

		scroll.animator = gfx.animator.new(scroll.duration, viewport.top, targetTop, scroll.easing)
	end

	if scroll.animator then
		-- Maintain cursor position in viewport during scroll
		local cursorX, cursorY = cursor:getPosition()
		local viewportY = cursorY - viewport.top

		viewport:moveTo(scroll.animator:currentValue())
		cursor:moveTo(cursorX, viewport.top + viewportY)

		if scroll.animator:ended() then
			scroll.animator = nil
		end
	end

	-- Right button to activate links
	if playdate.buttonJustPressed(playdate.kButtonRight) then
		for _, link in ipairs(page.links) do
			if cursor:alphaCollision(link) then
				fetchPage(link.url)
				break
			end
		end
	end


	-- B button to go back in history
	if playdate.buttonJustPressed(playdate.kButtonB) then
		if #history > 0 then
			local prevURL = table.remove(history)
			fetchPage(prevURL, true)
		end
	end
	
	-- rotate cursor if crank moves
	if playdate.getCrankChange() ~= 0 then
		cursor:updateImage()
	end

	-- UP to thrust cursor forward
	if playdate.buttonIsPressed(playdate.kButtonUp) then
		cursor.speed = math.min(cursor.maxSpeed, cursor.speed + cursor.thrust)
	else
		cursor.speed = cursor.speed * cursor.friction
	end

	local radians = math.rad(playdate.getCrankPosition() - 90)  -- Adjust for 0° being up
	local vx = math.cos(radians) * cursor.speed
	local vy = math.sin(radians) * cursor.speed

	if vx ~= 0 or vy ~= 0 then
		local cursorX, cursorY = cursor:getPosition()
		cursorX = cursorX + vx
		cursorY = math.min(page.height, math.max(0, cursorY + vy))

		-- Auto-scroll viewport to keep cursor visible
		if cursorY < viewport.top then
			viewport:moveTo(cursorY)
		end

		if cursorY > viewport.top + SCREEN_HEIGHT then
			viewport:moveTo(cursorY - SCREEN_HEIGHT)
		end

		-- Move cursor with collision detection, wrapping horizontally
		local _, _, collisions, _ = cursor:moveWithCollisions(cursorX % SCREEN_WIDTH, cursorY)
		if #collisions > 0 then
			for _, collision in ipairs(collisions) do
				collision.other:markDirty()
			end
		end

		-- Update hover state for all links
		for _, link in ipairs(page.links) do
			link:updateHoverState()
		end
	end

	gfx.sprite.update()
end
