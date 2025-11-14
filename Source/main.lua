import "CoreLibs/graphics"
import "CoreLibs/sprites"

local gfx = playdate.graphics
local geo = playdate.geometry

local fnt = gfx.font.new("fonts/Asheville-Sans-14-Bold")
gfx.setFont(fnt)

local testText, truncated = gfx.sprite.spriteWithText("Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est laborum.", 400, 400)

testText:moveTo(200, 120)
testText:add()

-- viewport
local viewportTop = 0

-- cursor parameters
local vx = 0
local vy = 0
local thrust = 0.5
local maxSpeed = 4
local friction = 0.95

local cursor = gfx.sprite.new()
cursor:moveTo(200, 120)
cursor:setSize(25, 25)
cursor:setZIndex(32767)
cursor:add()

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
			
	local crankChange = playdate.getCrankChange()
	if crankChange ~= 0 then
		cursor:markDirty()
	end

	if playdate.buttonIsPressed(playdate.kButtonUp) then
		local radians = math.rad(playdate.getCrankPosition() - 90)  -- Adjust for 0° being up
		
		vx = vx + math.cos(radians) * thrust
		vy = vy + math.sin(radians) * thrust
	end
	
	if vx ~= 0 or vy ~= 0 then
		vx = vx * friction
		vy = vy * friction
		
		local speed = math.sqrt(vx * vx + vy * vy)
		if speed > maxSpeed then
			vx = (vx / speed) * maxSpeed
			vy = (vy / speed) * maxSpeed
		end
		
		local curX, curY = cursor:getPosition()
		local toX = curX + vx
		local toY = math.max(0, curY + vy)
		
		if toY < viewportTop then
			gfx.setDrawOffset(0, 0 - toY)
			viewportTop = toY
		end
		
		if toY > viewportTop + 240 then
			gfx.setDrawOffset(0, 240 - toY)
			viewportTop = toY - 240
		end
		
		cursor:moveTo(toX % 400, toY)
	end

	gfx.sprite.update()
	
end