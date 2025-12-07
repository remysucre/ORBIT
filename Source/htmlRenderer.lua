-- html.lua
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

	local x = 0
	local y = 0
	local links = {}
	local currentLink = nil  -- {url, segments}

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

			-- Handle leading space
			if text:match("^%s") and x > 0 then
				x = x + font:getTextWidth(" ")
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
					x = x + font:getTextWidth(" ")
				end
			end

			-- Handle trailing space
			if text:match("%s$") then
				x = x + font:getTextWidth(" ")
			end
		end
	end

	-- Calculate page height
	local pageHeight = math.max(240, y + lineHeight + 30)

	-- Second pass: create image and draw text
	local pageImage = gfx.image.new(pageWidth, pageHeight, gfx.kColorWhite)
	gfx.pushContext(pageImage)
	gfx.setFont(font)

	for _, item in ipairs(layoutItems) do
		gfx.drawText(item.text, padding + item.x, padding + item.y)

		-- Draw underline for links
		if item.isLink then
			local underlineY = padding + item.y + lineHeight - 2
			gfx.drawLine(padding + item.x, underlineY,
			             padding + item.x + item.w, underlineY)
		end
	end

	gfx.popContext()

	-- Third pass: collect link segments
	local linkMap = {}  -- url -> {segments}
	for _, item in ipairs(layoutItems) do
		if item.isLink and item.url then
			if not linkMap[item.url] then
				linkMap[item.url] = {url = item.url, segments = {}}
			end
			table.insert(linkMap[item.url].segments, {
				x = item.x,
				y = item.y,
				w = item.w
			})
		end
	end

	-- Convert to array
	for _, linkData in pairs(linkMap) do
		table.insert(links, linkData)
	end

	return pageImage, pageHeight, links
end
