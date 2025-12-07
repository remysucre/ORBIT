-- siteparsers.lua
-- Site-specific DOM to IR converters for ORBIT
--
-- Each parser takes a DOM table (from json.decode(html.parse(htmlString)))
-- and returns an IR array: [{kind="plain"/"link"/"newline", text, url}, ...]

siteparsers = {}

-- =============================================================================
-- Helper Functions
-- =============================================================================

local entityMap = {
	["&nbsp;"] = " ",
	["&amp;"] = "&",
	["&lt;"] = "<",
	["&gt;"] = ">",
	["&quot;"] = "\"",
	["&#39;"] = "'",
	["&apos;"] = "'",
}

local function decodeEntities(text)
	if not text then return nil end

	-- Named entities
	text = text:gsub("&%w+;", function(entity)
		return entityMap[entity] or entity
	end)

	-- Numeric entities &#123; or &#x1F;
	text = text:gsub("&#x(%x+);", function(hex)
		local n = tonumber(hex, 16)
		if n and n < 128 then return string.char(n) end
		return ""
	end)
	text = text:gsub("&#(%d+);", function(dec)
		local n = tonumber(dec)
		if n and n < 128 then return string.char(n) end
		return ""
	end)

	return text
end

local function cleanText(text)
	if not text then return nil end

	local cleaned = text
		:gsub("\r\n", " ")
		:gsub("\n", " ")
		:gsub("\t", " ")
		:gsub("%s+", " ")  -- collapse multiple spaces

	cleaned = decodeEntities(cleaned)
	cleaned = cleaned:match("^%s*(.-)%s*$")  -- trim

	if cleaned and #cleaned > 0 then
		return cleaned
	end
	return nil
end

-- Extract all text content from a DOM node recursively
local function extractText(node)
	if not node then return "" end

	if node.text then
		return node.text
	end

	local result = ""
	if node.children then
		for _, child in ipairs(node.children) do
			result = result .. extractText(child)
		end
	end
	return result
end

-- Find first element matching a tag name
local function findByTag(node, tagName)
	if not node then return nil end

	if node.tag == tagName then
		return node
	end

	if node.children then
		for _, child in ipairs(node.children) do
			local found = findByTag(child, tagName)
			if found then return found end
		end
	end
	return nil
end

-- Find all elements matching a tag name
local function findAllByTag(node, tagName, results)
	results = results or {}
	if not node then return results end

	if node.tag == tagName then
		table.insert(results, node)
	end

	if node.children then
		for _, child in ipairs(node.children) do
			findAllByTag(child, tagName, results)
		end
	end
	return results
end

-- Find element by class (checks if class contains the value)
local function findByClass(node, className)
	if not node then return nil end

	if node.attrs and node.attrs.class then
		if node.attrs.class:match(className) then
			return node
		end
	end

	if node.children then
		for _, child in ipairs(node.children) do
			local found = findByClass(child, className)
			if found then return found end
		end
	end
	return nil
end

-- Find all elements by class
local function findAllByClass(node, className, results)
	results = results or {}
	if not node then return results end

	if node.attrs and node.attrs.class then
		if node.attrs.class:match(className) then
			table.insert(results, node)
		end
	end

	if node.children then
		for _, child in ipairs(node.children) do
			findAllByClass(child, className, results)
		end
	end
	return results
end

-- =============================================================================
-- NPR Text Frontpage Parser
-- =============================================================================

local function parseNPRFrontpage(dom)
	local ir = {}

	-- Add title
	table.insert(ir, {kind = "plain", text = "NPR"})
	table.insert(ir, {kind = "plain", text = " News"})
	table.insert(ir, {kind = "newline"})
	table.insert(ir, {kind = "newline"})

	-- Find all links in the main list
	local allLinks = findAllByTag(dom, "a")
	print("Found", #allLinks, "links")

	for _, link in ipairs(allLinks) do
		print("Link:", link.attrs and link.attrs.href or "no href")
		local href = link.attrs and link.attrs.href or ""

		-- Only include article links (start with / and contain story IDs)
		if href:match("^/[gn]") or href:match("^/nx") then
			local headline = cleanText(extractText(link))
			if headline and #headline > 0 then
				-- Make full URL
				local fullUrl = "https://text.npr.org" .. href

				table.insert(ir, {kind = "link", text = headline, url = fullUrl})
				table.insert(ir, {kind = "newline"})
				table.insert(ir, {kind = "newline"})
			end
		end
	end

	print("Frontpage IR elements:", #ir)
	return ir
end

-- =============================================================================
-- NPR Article Parser
-- =============================================================================

local function parseNPRArticle(dom)
	local ir = {}

	-- Find title (h1 with class story-title, or just h1)
	local titleNode = findByClass(dom, "story%-title") or findByTag(dom, "h1")
	if titleNode then
		local title = cleanText(extractText(titleNode))
		if title then
			table.insert(ir, {kind = "plain", text = title})
			table.insert(ir, {kind = "newline"})
			table.insert(ir, {kind = "newline"})
		end
	end

	-- Find date/byline
	local dateNode = findByClass(dom, "topic%-date")
	if dateNode then
		local dateText = cleanText(extractText(dateNode))
		if dateText then
			table.insert(ir, {kind = "plain", text = dateText})
			table.insert(ir, {kind = "newline"})
			table.insert(ir, {kind = "newline"})
		end
	end

	-- Find paragraphs in the paragraphs-container, or just all p tags
	local container = findByClass(dom, "paragraphs%-container")
	local paragraphs
	if container then
		paragraphs = findAllByTag(container, "p")
	else
		paragraphs = findAllByTag(dom, "p")
	end

	for _, p in ipairs(paragraphs) do
		-- Skip if it's part of meta/navigation
		local parentClass = ""

		local text = cleanText(extractText(p))
		if text and #text > 0 then
			table.insert(ir, {kind = "plain", text = text})
			table.insert(ir, {kind = "newline"})
			table.insert(ir, {kind = "newline"})
		end
	end

	print("IR elements:", #ir)
	return ir
end

-- =============================================================================
-- Parser Registry
-- =============================================================================

siteparsers.parsers = {
	{
		name = "NPR Frontpage",
		pattern = "^https?://text%.npr%.org/?$",
		parse = parseNPRFrontpage
	},
	{
		name = "NPR Article",
		pattern = "^https?://text%.npr%.org/.*",
		parse = parseNPRArticle
	}
}

-- Find a parser for a given URL
function siteparsers.findParser(url)
	for _, parser in ipairs(siteparsers.parsers) do
		if url:match(parser.pattern) then
			return parser
		end
	end
	return nil
end

-- Parse HTML for a given URL
-- Returns IR array or nil
function siteparsers.parse(url, htmlString)
	local parser = siteparsers.findParser(url)
	if not parser then
		return nil, "No parser for URL: " .. url
	end

	-- Parse HTML to DOM
	print("HTML input length:", #htmlString)
	local jsonStr = html.parse(htmlString)
	if not jsonStr then
		return nil, "Failed to parse HTML"
	end

	print("JSON length:", #jsonStr)

	local dom = json.decode(jsonStr)
	if not dom then
		return nil, "Failed to decode JSON"
	end

	print("DOM tag:", dom.tag or "none", "children:", dom.children and #dom.children or 0)
	-- Print first level children
	if dom.children then
		for i, child in ipairs(dom.children) do
			print("  Child", i, "tag:", child.tag or "text", "text:", child.text and child.text:sub(1, 50) or "nil")
		end
	end

	-- Run site-specific parser
	local ir = parser.parse(dom)
	if not ir or #ir == 0 then
		return nil, "Parser returned no content"
	end

	return ir
end
