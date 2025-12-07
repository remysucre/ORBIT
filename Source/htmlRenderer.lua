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

-- Render IR to page image and link data
-- Returns: pageImage, pageHeight, links (array of {url, segments})
function htmlRenderer.render(ir, font, pageWidth, padding)
	local contentWidth = pageWidth - 2 * padding
	local lineHeight = font:getHeight()
	local tracking = font:getTracking()
	-- Space width + tracking on both sides (since we draw words separately)
	local spaceWidth = font:getTextWidth(" ") + 2 * tracking

	local x = 0
	local y = 0

	-- First pass: layout all elements to calculate positions
	local layoutItems = {}  -- {text, x, y, w, isLink, url}

	for _, elem in ipairs(ir) do
		if elem.kind == "newline" then
			x = 0
			y = y + lineHeight
		elseif elem.kind == "plain" or elem.kind == "link" then
			local text = elem.text or ""
			local words = {}
			for word in text:gmatch("%S+") do
				table.insert(words, word)
			end

			-- Add space before this element if we're not at line start
			-- and the text has content (handles space between elements)
			if x > 0 and #words > 0 then
				x = x + spaceWidth
			end

			for i, word in ipairs(words) do
				local wordWidth = font:getTextWidth(word)

				-- Wrap if needed
				if x > 0 and x + wordWidth > contentWidth then
					x = 0
					y = y + lineHeight
				end

				-- Add layout item
				table.insert(layoutItems, {
					text = word,
					x = x,
					y = y,
					w = wordWidth,
					isLink = elem.kind == "link",
					url = elem.url
				})

				x = x + wordWidth

				-- Add space after word (except last)
				if i < #words then
					x = x + spaceWidth
				end
			end
		end
	end

	-- Calculate page height
	local pageHeight = math.max(240, y + lineHeight + 30)

	-- Collect link segments BEFORE drawing, so we can merge them per line
	local linkMap = {}  -- url -> {segments by line}
	for _, item in ipairs(layoutItems) do
		if item.isLink and item.url then
			if not linkMap[item.url] then
				linkMap[item.url] = {url = item.url, lineSegments = {}}
			end
			-- Group by y position (line)
			local lineKey = item.y
			if not linkMap[item.url].lineSegments[lineKey] then
				linkMap[item.url].lineSegments[lineKey] = {x = item.x, y = item.y, w = item.w}
			else
				-- Extend existing segment on this line
				local seg = linkMap[item.url].lineSegments[lineKey]
				local newRight = item.x + item.w
				seg.w = newRight - seg.x
			end
		end
	end

	-- Second pass: create image and draw text (transparent background)
	local pageImage = gfx.image.new(pageWidth, pageHeight, gfx.kColorClear)
	gfx.pushContext(pageImage)
	gfx.setFont(font)

	for _, item in ipairs(layoutItems) do
		gfx.drawText(item.text, padding + item.x, padding + item.y)
	end

	-- Don't draw underlines here - Link sprites handle them
	gfx.popContext()

	-- Convert linkMap to array format with segments array
	local links = {}
	for _, linkData in pairs(linkMap) do
		local segments = {}
		for _, seg in pairs(linkData.lineSegments) do
			table.insert(segments, {x = seg.x, y = seg.y, w = seg.w})
		end
		table.insert(links, {url = linkData.url, segments = segments})
	end

	return pageImage, pageHeight, links
end
