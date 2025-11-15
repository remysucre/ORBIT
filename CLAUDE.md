# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ORBIT is a Playdate game that implements a unique document/web browser-like interface. Users navigate content using a physics-based cursor controlled by the crank (for direction) and buttons (for thrust and scrolling).

## Build and Run

**Build the project:**
```bash
pdc Source ORBIT.pdx
```

This compiles the Lua source code in `Source/` into the `ORBIT.pdx` bundle.

**Requirements:**
- Playdate SDK must be installed
- `pdc` (Playdate Compiler) must be in PATH
- `PLAYDATE_SDK_PATH` environment variable should be set

## Architecture

### Core Systems

**Layout Engine (lines 50-143)**
- Parses JSON content structure with elements of type: `plain`, `link`, `vspace`
- Performs text wrapping based on `page.width` (380px)
- Generates drawable commands and collision sprites for links
- Outputs to `pageImage` which is set on the page sprite

**Cursor System (lines 36-48, 203-294)**
- Physics-based movement with thrust, max speed, and friction
- Direction controlled by crank position (converted to radians)
- UP button applies thrust; release applies friction
- Collision detection with link sprites to show hover state
- Cursor wraps horizontally (modulo 400) but is clamped vertically to page bounds

**Scrolling System (lines 17-21, 224-254)**
- D-pad DOWN/UP trigger animated scrolling
- Uses `playdate.graphics.animator` with easing function
- Maintains cursor position in viewport during scroll
- Auto-scrolls viewport when cursor moves beyond visible bounds

**Link Interaction (lines 12-15, 88-95, 117-142)**
- Links are created as sprites with collision rectangles
- Collision with cursor group triggers underline thickening
- Link sprites positioned based on layout engine output
- Currently stores `text` and `url` but URL navigation not implemented

### Content Structure

Content is defined as JSON with element types:
- `plain`: Regular text that wraps
- `link`: Clickable text with `url` property
- `vspace`: Vertical spacing (default 30px)

### Key Constants

- `page.width`: 380px (400 - 2×10 padding)
- `page.tail`: 30px extra scrollable space at bottom
- `scrollDist`: 220px per D-pad press
- `cursor.maxSpeed`: 8px per frame
- Font: Asheville-Sans-14-Bold

## File Structure

- `Source/main.lua` - All game logic (single file)
- `Source/fonts/` - Playdate font files (.fnt + .png)
- `ORBIT.pdx/` - Compiled output (gitignored)

## Development Notes

- The viewport system uses `gfx.setDrawOffset()` to implement scrolling
- Cursor position is in absolute coordinates; viewport offset is subtracted for rendering
- All UI is sprite-based using Playdate's sprite collision system
- The layout engine is single-pass and pre-renders the entire page to an image
