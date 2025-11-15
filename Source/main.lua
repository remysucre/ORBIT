import "CoreLibs/graphics"
import "CoreLibs/sprites"

local gfx = playdate.graphics
local geo = playdate.geometry
local net = playdate.network

local pageHeight = 0

local fnt = gfx.font.new("fonts/Asheville-Sans-14-Bold")
gfx.setFont(fnt)

-- d pad scrolling
local scrollAnimator = nil
local scrollEasing = playdate.easingFunctions.outQuint
local scrollDist = 220
local scrollDura = 400

-- viewport
local viewportTop = 0

local page = gfx.sprite.new()
page:setSize(100, 100)
page:moveTo(200, 120)
page:add()

page.padding = 10
page.width = 400 - 2 * page.padding
page.tail = 30
page.linkSprites = {}
page.hoveredLink = nil

-- cursor
local cursor = gfx.sprite.new()
cursor:moveTo(200, 120)
cursor:setSize(25, 25)
cursor:setZIndex(32767)
cursor:setCollideRect( 7, 7, 11, 11 )
cursor:setGroups({1})
cursor.collisionResponse = gfx.sprite.kCollisionTypeOverlap
cursor:add()

cursor.speed = 0
cursor.thrust = 0.5
cursor.maxSpeed = 8
cursor.friction = 0.85

-- networking
local httpConn = nil
local httpData = ""
local isLoading = false
local frameCount = 0
local statusMessage = nil
local accessRequested = false
local pageRequested = false

function parseURL(url)
	local secure = string.match(url, "^https://") ~= nil
	local host = string.match(url, "^https?://([^/]+)")
	local path = string.match(url, "^https?://[^/]+(.*)") or "/"
	local port = secure and 443 or 80
	return host, port, secure, path
end

function fetchPage(url)
	if isLoading then
		return
	end

	isLoading = true
	httpData = ""
	statusMessage = "Loading..."

	local host, port, secure, path = parseURL(url)

	httpConn = net.http.new(host, port, secure, "ORBIT")

	if not httpConn then
		statusMessage = "Network access denied"
		isLoading = false
		return
	end

	httpConn:setConnectTimeout(10)

	httpConn:setRequestCallback(function()
		if not httpConn then return end
		local bytes = httpConn:getBytesAvailable()
		if bytes > 0 then
			local chunk = httpConn:read(bytes)
			if chunk then
				httpData = httpData .. chunk
			end
		end
	end)

	httpConn:setRequestCompleteCallback(function()
		if not httpConn then return end
		local err = httpConn:getError()
		if err and err ~= "Connection closed" then
			statusMessage = "Error: " .. err
			isLoading = false
			httpConn = nil
			return
		end

		-- Parse JSON and layout
		local success, result = pcall(json.decode, httpData)
		if not success or not result or not result.content then
			statusMessage = "Failed to load page"
			isLoading = false
			httpConn = nil
			return
		end

		layout(result)
		statusMessage = nil

		isLoading = false
		httpConn = nil
	end)

	httpConn:get(path)
end

function layout(orb)
	if not orb or not orb.content then
		return
	end

	-- Remove old link sprites
	for _, linkSprite in ipairs(page.linkSprites) do
		if linkSprite then
			linkSprite:remove()
		end
	end
	page.linkSprites = {}
	page.hoveredLink = nil

	-- Reset viewport to top
	viewportTop = 0
	gfx.setDrawOffset(0, 0)

	-- Reset cursor position
	cursor:moveTo(200, 120)
	cursor.speed = 0

	local content = orb.content
	local x = 0
	local y = 0
	local h = fnt:getHeight()
	local toDraw = {}
	local links = {}

	for i, element in ipairs(orb.content) do
		if element and element.type and (element.type == "plain" or element.type == "link") then
			if element.text and type(element.text) == "string" then
				local x0 = x
				local y0 = y
				for word in string.gmatch(element.text, "%S+") do
					local w = fnt:getTextWidth(word)
					if x + w > page.width then
						y += h
						x = 0
						x0 = 0
						y0 = y
					end
					table.insert(toDraw, {
						txt = word,
						x = x,
						y = y
					})
					x += w

					local sw = fnt:getTextWidth(" ")
					if x + sw <= page.width then
						table.insert(toDraw, {
							txt = " ",
							x = x,
							y = y
						})
						x += sw
					end
				end

				if element.type == "link" then
					table.insert(links, {
						text = element.text,
						url = element.url or "",
						x = x0,
						y = y0
					})
				end
			end

		elseif element.type == "vspace" then
			x = 0
			y += element.vspace or 30
		end
	end
	
	
	pageHeight = math.max(240, y + 2 * page.padding)

	local pageImage = gfx.image.new(page.width, pageHeight)
	if not pageImage then
		return
	end

	gfx.pushContext(pageImage)

	for _, cmd in ipairs(toDraw) do
		if cmd and cmd.txt then
			gfx.drawText(cmd.txt, cmd.x, cmd.y)
		end
	end

	gfx.popContext()
	page:setImage(pageImage)
	page:moveTo(200, page.padding + pageHeight / 2)

	for _, link in ipairs(links) do
		if link and link.text and link.url then
			local l = gfx.sprite.new()
			local textWidth = fnt:getTextWidth(link.text)
			local textHeight = fnt:getHeight()
			l:setSize(textWidth, textHeight)
			l:setCollideRect(0, 0, textWidth, textHeight)
			l:setCollidesWithGroups({1})
			l.collisionResponse = gfx.sprite.kCollisionTypeOverlap

			local w, h = textWidth, textHeight

			function l:draw(x, y, width, height)
				-- links can only collide with the cursor
				if #self:overlappingSprites() > 0 then
					local lw = gfx.getLineWidth()
					gfx.setLineWidth(2)
					gfx.drawLine(0, h-2, w, h-2)
					gfx.setLineWidth(lw)
				else
					gfx.drawLine(0, h-2, w, h-2)
				end
			end

			l:moveTo(page.padding + link.x + w/2, page.padding + link.y + h/2)
			l.text = link.text
			l.url = link.url

			l:add()
			table.insert(page.linkSprites, l)
		end
	end
end

function cursor:draw(x, y, width, height)
	local w, h = self:getSize()
	local moon = geo.point.new(math.floor(w/2)+1, h-3)
	local tran = geo.affineTransform.new()

	tran:rotate(playdate.getCrankPosition(), math.floor(w/2)+1, math.floor(h/2)+1)
	tran:transformPoint(moon)

	local x, y = moon:unpack()

	playdate.graphics.setColor(playdate.graphics.kColorWhite)
	gfx.fillRect(x-4, y-4, 7, 7)
	gfx.fillRect(7, 7, 11, 11)

	playdate.graphics.setColor(playdate.graphics.kColorBlack)
	gfx.fillRect(x-2, y-2, 3, 3)
	gfx.fillRect(9, 9, 7, 7)
end

function playdate.update()

	frameCount += 1

	-- Request network access on first frame
	if frameCount == 1 and not accessRequested then
		net.http.requestAccess()
		accessRequested = true
	end

	-- Load initial page once after access is requested
	if accessRequested and not pageRequested then
		fetchPage("https://remy.wang/index.json")
		pageRequested = true
	end

	-- Display status message if present
	if statusMessage then
		gfx.clear()
		gfx.drawTextInRect(statusMessage, 20, 100, 360, 140)
	end

	-- scrolling page with D pad
	local scrollTarget = nil

	if playdate.buttonJustPressed(playdate.kButtonDown) then
		scrollTarget = math.min(pageHeight + page.tail - 240, viewportTop + scrollDist)
	end

	if playdate.buttonJustPressed(playdate.kButtonLeft) then
		scrollTarget = math.max(0, viewportTop - scrollDist)
	end

	if scrollTarget then
		scrollAnimator = gfx.animator.new(scrollDura, viewportTop, scrollTarget, scrollEasing)
	end

	if scrollAnimator then
		-- to maintain cursor position in view
		local x, y = cursor:getPosition()
		local viewY = y - viewportTop

		viewportTop = scrollAnimator:currentValue()

		gfx.setDrawOffset(0, 0 - viewportTop)

		cursor:moveTo(x, viewportTop + viewY)

		if scrollAnimator:ended() then
			scrollAnimator = nil
		end
	end

	-- rotate cursor if crank moves
	if playdate.getCrankChange() ~= 0 then
		cursor:markDirty()
	end

	-- A button to activate links
	if playdate.buttonJustPressed(playdate.kButtonA) then
		local overlapping = cursor:overlappingSprites()
		for _, sprite in ipairs(overlapping) do
			if sprite.url then
				fetchPage(sprite.url)
				break
			end
		end
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
		y = math.min(pageHeight + page.tail, math.max(0, y + vy))

		if y < viewportTop then
			gfx.setDrawOffset(0, 0 - y)
			viewportTop = y
		end

		if y > viewportTop + 240 then
			gfx.setDrawOffset(0, 240 - y)
			viewportTop = y - 240
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