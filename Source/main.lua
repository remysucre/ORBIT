import "CoreLibs/graphics"
import "CoreLibs/sprites"

local gfx = playdate.graphics
local geo = playdate.geometry

local pageHeight = 0
local links = {}
local hoveredLink = nil

local fnt = gfx.font.new("fonts/Asheville-Sans-14-Bold")
gfx.setFont(fnt)

-- viewport
local viewportTop = 0

local page = gfx.sprite.new()
page:setSize(100, 100)
page:moveTo(200, 120)
page:add()

page.padding = 10
page.width = 400 - 2 * page.padding
page.tail = 30

-- cursor
local cursor = gfx.sprite.new()
cursor:moveTo(200, 120)
cursor:setSize(25, 25)
cursor:setZIndex(32767)
cursor:add()

cursor.speed = 0
cursor.thrust = 0.5
cursor.maxSpeed = 8
cursor.friction = 0.85

function layout(orb)
		
	local content = orb.content
	local x = 0
	local y = 0
	local toDraw = {}
	
	for _, element in ipairs(orb.content) do
		if element.type == "plain" then
			for word in string.gmatch(element.text, "%S+") do
				local w, h = gfx.getTextSize(word)
				if x + w > page.width then
					y += h
					x = 0
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
		elseif element.type == "vspace" then
			x = 0
			y += element.vspace or 30
		end
	end
	
	
	pageHeight = y + 2 * page.padding
	
	local pageImage = gfx.image.new(page.width, pageHeight)
	gfx.lockFocus(pageImage)
	
	for _, cmd in ipairs(toDraw) do
		gfx.drawText(cmd.txt, cmd.x, cmd.y)
	end
	
	gfx.unlockFocus()
	page:setImage(pageImage)
	page:moveTo(200, page.padding + pageHeight / 2)
end

local orb = json.decode([[
	{
		"content": [
			{ 
				"type": "plain",
				"text": "Lorem ipsum dolor sit amet, consectetur adipiscing elit. Pellentesque pellentesque mi at dignissim pharetra. Ut volutpat eu velit at lacinia. Vivamus scelerisque fringilla sapien, in imperdiet turpis. Integer eget metus eu purus tincidunt semper quis eu ipsum. Duis at lorem ut est bibendum facilisis. In vel varius erat. Donec vel lacus laoreet, condimentum orci vitae, tincidunt nisl. Lorem ipsum dolor sit amet, consectetur adipiscing elit. Morbi tempus tincidunt tellus, ut dictum velit convallis in. Nunc egestas rutrum dolor, id hendrerit nisl ullamcorper tempor."
			},
			{
				"type": "vspace"
			},
			{
				"type": "plain",
				"text": "Vestibulum imperdiet condimentum scelerisque. Nullam efficitur placerat mi id venenatis. Nulla nibh neque, scelerisque in elit sit amet, hendrerit dignissim risus. Cras rhoncus, ligula eget sollicitudin molestie, nibh arcu mollis sapien, vel aliquet lacus mi et sem. Quisque varius diam sed dui ultrices, quis congue nunc pellentesque. Suspendisse in augue odio. Quisque porttitor erat eget feugiat aliquet. Donec consectetur lacus at felis posuere, at lacinia eros volutpat. Morbi porttitor interdum libero, vitae vehicula est varius quis. Integer vulputate eget leo nec ultrices. Fusce tempus feugiat felis. Donec ornare lacus leo, non bibendum velit vestibulum et. Nunc sed scelerisque ligula. Cras bibendum scelerisque auctor. Quisque a eros consectetur, vehicula ex eu, lobortis est."
			},
			{
				"type": "vspace"
			},
			{
				"type": "plain",
				"text": "Nam at magna sit amet ipsum dignissim ornare. Morbi congue turpis id malesuada pharetra. Ut eget libero mauris. Donec dapibus cursus arcu quis suscipit. In interdum interdum ipsum quis feugiat. Nam rhoncus augue nisl, a euismod elit finibus vitae. Morbi ullamcorper lorem ante. Mauris eu rutrum velit. Aenean neque dui, ornare interdum quam ut, accumsan tempus libero. Orci varius natoque penatibus et magnis dis parturient montes, nascetur ridiculus mus. Proin quis porta dui, nec volutpat enim. Etiam tempus quam dolor, at vehicula sapien posuere condimentum. Morbi imperdiet a nisi ac vestibulum. Phasellus dictum neque sem, et viverra purus ultricies vitae. Mauris non justo vel enim tempor finibus vitae a mauris."
			},
			{
				"type": "vspace"
			},
			{
				"type": "plain",
				"text": "Nullam quis odio consectetur ipsum congue consectetur ac a leo. Vivamus blandit rutrum elit eu volutpat. Donec eu cursus lectus, quis feugiat augue. Pellentesque ac feugiat nulla. Suspendisse vitae dapibus felis, quis vulputate lectus. Class aptent taciti sociosqu ad litora torquent per conubia nostra, per inceptos himenaeos. Vestibulum ante ipsum primis in faucibus orci luctus et ultrices posuere cubilia curae; Integer non luctus ligula, sit amet aliquam turpis. Duis in leo libero."
			},
			{
				"type": "vspace"
			},
			{
				"type": "plain",
				"text": "Sed eget luctus nisi. Vivamus ac condimentum lorem, ut volutpat turpis. Donec volutpat nibh id metus egestas dictum. Donec varius nibh at lobortis viverra. Phasellus posuere rutrum nisl maximus molestie. Suspendisse a dignissim purus, eget pellentesque turpis. Proin quis lobortis mi. Nunc blandit enim velit, vel convallis nunc dictum vel. Nunc ultricies tincidunt dui. Fusce velit elit, bibendum a nisi vel, feugiat blandit mi. Donec maximus congue ligula ac blandit. Nunc in luctus dolor. Sed sed lorem porta, placerat tellus vitae, luctus nunc. Integer tincidunt, tortor tincidunt ultrices imperdiet, enim arcu consequat eros, nec elementum eros ante et enim."
			}
			]
	}
	]])

layout(orb)

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

	if playdate.getCrankChange() ~= 0 then
		cursor:markDirty()
	end

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
		
		cursor:moveTo(x % 400, y)
	end

	gfx.sprite.update()	
end