Welcome to ORBIT, a web browser for Playdate

![ORBIT demo](demo.gif)

Follow [instructions](https://help.play.date/games/sideloading/) to upload [`ORBIT.pdx`](https://orbit.casa/ORBIT.pdx.zip) to your device.

**CONTROLS**

- CRANK to aim cursor direction
- UP to thrust cursor forward
- LEFT/DOWN to scroll up/down by 1 page
- RIGHT/A to activate link (when hovering)

**ADDING SAVED PAGES**

Connect your playdate to a computer with USB and follow [instructions](https://help.play.date/games/sideloading/#data-disk-mode) to enter Data Disk mode. Then, in the Data folder on the PLAYDATE disk, you should see a folder that ends with "orbit". Open it, and edit the file favorites.json to add links.

**CONTRIBUTING**

There are many ways to contribute to ORBIT, including writing your own web page, adding a site parser for an HTML page, or even drawing missing fonts.

**WRITING WEB PAGES FOR ORBIT**

ORBIT currently supports two formats, markdown and HTML. If you are writing your own page from scratch, you should do it in markdown. ORBIT uses the cmark library from the Commonmark project to parse markdowns, so you can refer to commonmark.org for the syntax. Currently we only render plain text and links, and PRs are welcome to support other elements.

**ADDING SITE PARSERS**

Because of limitations of the Playdate console, ORBIT cannot (and never will) support arbitrary websites. Instead, we implement a novel "exo browser" architecture: there is a curated set of custom rules that parse a selected set of websites, and you can contribute by writing more rules. While there is plan to support images in the future, ORBIT focuses on plain text content like news articles and (non-technical) blogs. You can see some example site parsers [here](https://github.com/remysucre/ORBIT/blob/main/Source/siteparsers.lua).

**DRAWING MISSING FONTS**

ORBIT uses a hand-drawn font called [cuniform](https://github.com/remysucre/cuniform) designed specifically for the playdate. It covers most of the common characters, but you might be surprised how many characters there are out there! Whenever you see �, it means that character is missing in our font. You can figure out what character it is by visiting the same page on your computer, and use https://play.date/caps/ to draw your favorite design.


**ACKNOWLEDGEMENT**

ORBIT took inspiration from [HYPER METEOR](https://play.date/games/hyper-meteor/)
and the [Constellation Browser](https://particlestudios.itch.io/constellation-browser).
Markdown parsing is done with [cmark](https://github.com/commonmark/cmark), and HTML parsing is done with [lexbor](https://github.com/lexbor/lexbor).
