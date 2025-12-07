-- htmlRenderer.lua
-- HTML rendering support for ORBIT
--
-- IR (Intermediate Representation) format:
--   { kind = "plain", text = "Hello world" }
--   { kind = "link", text = "Click here", url = "https://..." }
--   { kind = "newline" }
--
-- Frontend: site-specific parsers convert DOM -> IR
-- Backend: renderer converts IR -> page image + link positions

local gfx = playdate.graphics

htmlRenderer = {}

-- =============================================================================
-- Backend Renderer
-- =============================================================================

-- Word-wrap layout algorithm (mirrors C layoutWords function)
-- Returns: segments array, endX, endY
local function layoutWords(text, startX, startY, font, contentWidth, tracking)
	local segments = {}
	local x = startX
	local y = startY
	local h = font:getHeight()

	-- Space width without tracking (we add tracking manually like C does)
	local spaceWidth = font:getTextWidth(" ")

	-- Current segment being built
	local segText = ""
	local segX, segY = x, y

	local pos = 1
	local len = #text

	while pos <= len do
		local char = text:sub(pos, pos)

		if char == " " then
			-- Handle space
			if x + spaceWidth <= contentWidth then
				x = x + spaceWidth + tracking
				segText = segText .. " "
			end
			pos = pos + 1
		else
			-- Find word end
			local wordEnd = pos
			while wordEnd <= len and text:sub(wordEnd, wordEnd) ~= " " do
				wordEnd = wordEnd + 1
			end

			-- Extract word
			local word = text:sub(pos, wordEnd - 1)
			local wordWidth = font:getTextWidth(word)

			-- Wrap if needed
			if x > 0 and x + wordWidth > contentWidth then
				-- Save current segment
				if #segText > 0 then
					table.insert(segments, {
						text = segText,
						x = segX,
						y = segY,
						w = font:getTextWidth(segText)
					})
				end

				-- Start new line
				y = y + h
				x = 0
				segText = ""
				segX = x
				segY = y
			end

			-- Add word to segment (add tracking after word like C does)
			x = x + wordWidth + tracking
			segText = segText .. word
			pos = wordEnd
		end
	end

	-- Save final segment
	if #segText > 0 then
		table.insert(segments, {
			text = segText,
			x = segX,
			y = segY,
			w = font:getTextWidth(segText)
		})
	end

	return segments, x, y
end

-- Render IR to page image and link data
-- Returns: pageImage, pageHeight, links (array of {url, segments})
function htmlRenderer.render(ir, font, pageWidth, padding)
	local contentWidth = pageWidth - 2 * padding
	local lineHeight = font:getHeight()
	local tracking = font:getTracking()
	local spaceWidth = font:getTextWidth(" ")

	local x = 0
	local y = 0

	-- Layout pass: collect all text segments and links
	local allSegments = {}  -- {text, x, y, w}
	local links = {}  -- {url, segments}

	for _, elem in ipairs(ir) do
		if elem.kind == "newline" then
			x = 0
			y = y + lineHeight
		elseif elem.kind == "plain" or elem.kind == "link" then
			local text = elem.text or ""
			if #text > 0 then
				-- Add space before this element if continuing on same line
				if x > 0 then
					x = x + spaceWidth + tracking
				end

				local segments, newX, newY = layoutWords(text, x, y, font, contentWidth, tracking)

				-- Add segments to drawing list
				for _, seg in ipairs(segments) do
					table.insert(allSegments, seg)
				end

				-- Add link entry directly
				if elem.kind == "link" and elem.url then
					local linkSegs = {}
					for _, seg in ipairs(segments) do
						table.insert(linkSegs, {x = seg.x, y = seg.y, w = seg.w})
					end
					table.insert(links, {url = elem.url, segments = linkSegs})
				end

				x = newX
				y = newY
			end
		end
	end

	-- Calculate page height
	local pageHeight = math.max(240, y + lineHeight + 30)

	-- Create image and draw text
	local pageImage = gfx.image.new(pageWidth, pageHeight, gfx.kColorClear)
	gfx.pushContext(pageImage)
	gfx.setFont(font)

	for _, seg in ipairs(allSegments) do
		gfx.drawText(seg.text, padding + seg.x, padding + seg.y)
	end

	gfx.popContext()

	return pageImage, pageHeight, links
end
