import "CoreLibs/graphics"
import "CoreLibs/sprites"

local gfx = playdate.graphics
local geo = playdate.geometry
local net = playdate.network

local fnt = gfx.font.new("fonts/SYSTEM6")
gfx.setFont(fnt)

-- d pad scrolling
local scrollAnimator = nil
local scrollEasing = playdate.easingFunctions.outQuint
local scrollDist = 220
local scrollDura = 400

-- history stack for back navigation
local history = {}
local currentURL = nil

-- viewport
local viewport = {
	top = 0
}

function viewport:moveTo(top)
	self.top = top
	gfx.setDrawOffset(0, -self.top)
end

-- page
local page = gfx.sprite.new()
page:setSize(100, 100)
page:moveTo(200, 120)
page:add()

page.height = 0
page.width = 400
page.padding = 10
page.contentWidth = page.width - 2 * page.padding
page.linkSprites = {}
page.hoveredLink = nil

-- cursor
local cursor = gfx.sprite.new()
cursor:moveTo(130, 42)
cursor:setSize(25, 25)
cursor:setZIndex(32767)
cursor:setCollideRect( 7, 7, 11, 11 )
cursor:setGroups({1})
cursor:add()

cursor.collisionResponse = gfx.sprite.kCollisionTypeOverlap
cursor.speed = 0
cursor.thrust = 0.5
cursor.maxSpeed = 8
cursor.friction = 0.85

function parseURL(url)
	local secure = string.match(url, "^https://") ~= nil
	local host = string.match(url, "^https?://([^/]+)")
	local path = string.match(url, "^https?://[^/]+(.*)") or "/"
	local port = secure and 443 or 80
	return host, port, secure, path
end

function fetchPage(url, isBack)
	-- Push current URL to history before navigating (unless going back)
	if not isBack and currentURL then
		table.insert(history, currentURL)
	end

	-- Clear screen and show loading message
	gfx.clear()
	gfx.setDrawOffset(0, 0)
	gfx.drawTextInRect("Loading...", 20, 100, 360, 140)

	-- Flush display to show the loading message
	playdate.display.flush()

	local host, port, secure, path = parseURL(url)

	local httpConn = net.http.new(host, port, secure, "ORBIT")

	if not httpConn then
		gfx.clear()
		gfx.drawTextInRect("Network access denied", 20, 100, 360, 140)
		return
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
		gfx.clear()
		gfx.drawTextInRect("Error: " .. err, 20, 100, 360, 140)
		return
	end

	-- Render md0 content
	local success, err = pcall(render, httpData)
	if not success then
		gfx.clear()
		gfx.drawTextInRect("Failed to render page: " .. tostring(err), 20, 100, 360, 140)
		return
	end

	-- Update current URL after successful render
	currentURL = url
end

function render(text)
	-- Remove old link sprites
	for _, linkSprite in ipairs(page.linkSprites) do
		if linkSprite then
			linkSprite:remove()
		end
	end
	page.linkSprites = {}
	page.hoveredLink = nil

	-- Stop cursor momentum
	cursor.speed = 0

	-- Preserve cursor's screen-relative position
	local cursorX, cursorY = cursor:getPosition()
	local screenY = cursorY - viewport.top

	-- Reset viewport to top
	viewport:moveTo(0)

	local h = fnt:getHeight()
	local x, y = 0, 0
	local toDraw = {}
	local linkRefs = {}

	-- Helper to create link sprite
	local function createLinkSprite(text, url, lx, ly, width)
		local l = gfx.sprite.new()
		local textHeight = fnt:getHeight()
		l:setSize(width, textHeight)
		l:setCollideRect(0, 0, width, textHeight)
		l:setCollidesWithGroups({1})
		l.collisionResponse = gfx.sprite.kCollisionTypeOverlap

		local w, h = width, textHeight

		function l:draw(x, y, width, height)
			local lw = gfx.getLineWidth()
			if #self:overlappingSprites() > 0 then
				gfx.setLineWidth(2)
			end
			gfx.drawLine(0, h-2, w, h-2)
			gfx.setLineWidth(lw)
		end

		l:moveTo(page.padding + lx + w/2, page.padding + ly + h/2)
		l.text = text
		l.url = url

		l:add()
		table.insert(page.linkSprites, l)
	end

	-- Process all lines in single pass
	for line in string.gmatch(text .. "\n", "([^\n]*)\n") do
		-- Check if this is a link definition
		local num, url = string.match(line, "^%[(%d+)%]:%s+(.+)$")
		if num and url then
			-- Create sprites for all refs to this link
			local n = tonumber(num)
			if linkRefs[n] then
				for _, ref in ipairs(linkRefs[n]) do
					createLinkSprite(ref.text, url, ref.x, ref.y, ref.width)
				end
			end
		else
			-- Content line (blank or not)
			-- Process line word by word
			for token in string.gmatch(line, "%S+") do
				-- Try to match [word][n] with optional trailing characters
				local word, num, trailing = string.match(token, "^%[(%S+)%]%[(%d+)%](.*)$")

				if word and num then
					-- Render link text
					local x0, y0 = x, y
					local w = fnt:getTextWidth(word)

					if x + w > page.contentWidth then
						y += h
						x = 0
						x0, y0 = x, y
					end

					table.insert(toDraw, {txt = word, x = x, y = y})
					x += w

					-- Save link reference
					local n = tonumber(num)
					if not linkRefs[n] then
						linkRefs[n] = {}
					end
					table.insert(linkRefs[n], {
						text = word,
						x = x0,
						y = y0,
						width = w
					})

					-- Render trailing characters if any
					if trailing and #trailing > 0 then
						local tw = fnt:getTextWidth(trailing)

						if x + tw > page.contentWidth then
							y += h
							x = 0
						end

						table.insert(toDraw, {txt = trailing, x = x, y = y})
						x += tw
					end
				else
					-- Plain word
					local w = fnt:getTextWidth(token)

					if x + w > page.contentWidth then
						y += h
						x = 0
					end

					table.insert(toDraw, {txt = token, x = x, y = y})
					x += w
				end

				-- Add space after word
				local sw = fnt:getTextWidth(" ")
				if x + sw <= page.contentWidth then
					table.insert(toDraw, {txt = " ", x = x, y = y})
					x += sw
				end
			end

			-- Move to next line
			x = 0
			y += h
		end
	end

	local contentHeight = y + h
	page.height = math.max(240, contentHeight + 2 * page.padding)

	local pageImage = gfx.image.new(page.width, page.height)
	if not pageImage then
		return
	end

	gfx.pushContext(pageImage)

	for _, cmd in ipairs(toDraw) do
		if cmd and cmd.txt then
			fnt:drawText(cmd.txt, page.padding + cmd.x, page.padding + cmd.y)
		end
	end

	gfx.popContext()
	page:setImage(pageImage)
	page:moveTo(200, page.height / 2)

	-- Set cursor to same screen position in new page
	local newY = math.min(screenY, page.height)
	cursor:moveTo(cursorX, newY)
end

function cursor:draw(x, y, width, height)
	local w, h = self:getSize()
	local moon = geo.point.new(math.floor(w/2)+1, h-3)
	local tran = geo.affineTransform.new()

	tran:rotate(playdate.getCrankPosition(), math.floor(w/2)+1, math.floor(h/2)+1)
	tran:transformPoint(moon)

	local x, y = moon:unpack()

	playdate.graphics.setColor(playdate.graphics.kColorWhite)
	gfx.fillRect(x-3, y-3, 5, 5)
	gfx.fillRect(8, 8, 9, 9)

	playdate.graphics.setColor(playdate.graphics.kColorBlack)
	gfx.fillRect(x-2, y-2, 3, 3)
	gfx.fillRect(9, 9, 7, 7)
end

-- load homepage when app starts
local pageRequested = false

function playdate.update()

	if not pageRequested then
		fetchPage("https://orbit.casa/tutorial.md")
		pageRequested = true
	end

	-- scrolling page with D pad
	local downPressed = playdate.buttonJustPressed(playdate.kButtonDown)
	local leftPressed = playdate.buttonJustPressed(playdate.kButtonLeft)
	local targetTop = nil
	
	if downPressed then
		targetTop = math.min(math.max(page.height - 240, 0), viewport.top + scrollDist)
	end

	if leftPressed then
		targetTop = math.max(0, viewport.top - scrollDist)
	end

	if targetTop then
		assert(downPressed or leftPressed, "targetTop should only be set once right after button presses")
		scrollAnimator = gfx.animator.new(scrollDura, viewport.top, targetTop, scrollEasing)
	end

	if scrollAnimator then
		-- to maintain cursor position in view
		local x, y = cursor:getPosition()
		local viewY = y - viewport.top

		viewport:moveTo(scrollAnimator:currentValue())
		cursor:moveTo(x, viewport.top + viewY)

		if scrollAnimator:ended() then
			scrollAnimator = nil
		end
	end

	-- A button to activate links
	if playdate.buttonJustPressed(playdate.kButtonRight) then
		local overlapping = cursor:overlappingSprites()
		for _, sprite in ipairs(overlapping) do
			if sprite.url then
				fetchPage(sprite.url)
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
		cursor:markDirty()
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

		local x, y = cursor:getPosition()
		x += vx
		y = math.min(page.height, math.max(0, y + vy))

		if y < viewport.top then
			viewport:moveTo(y)
		end

		if y > viewport.top + 240 then
			viewport:moveTo(y - 240)
		end

		local _, _, cols, _ = cursor:moveWithCollisions(x % 400, y)
		if #cols > 0 then
			for _, col in ipairs(cols) do
				col.other:markDirty()
			end
		end
	end

	gfx.sprite.update()
end