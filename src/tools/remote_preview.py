#!/usr/bin/env python3
"""Bundle the Remote UI into ONE self-contained HTML file with a mock bridge.

The shipped page is web_ui.html plus assets/, served by Schwung Manager next
to its own bridge script. To look at it with no device -- in a browser, or
published as a review page -- inline everything and swap the bridge for
src/remote/dev/mock-bridge.js, which plays the manager's part from memory.

    python3 src/tools/remote_preview.py [out.html]
"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "src/remote")
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build-preview/je8086-remote-preview.html")

html = open(os.path.join(SRC, "web_ui.html")).read()
def read(rel): return open(os.path.join(SRC, rel)).read()

css = read("assets/style.css")
# The manager's fonts are same-origin on the device; for a standalone preview
# take the same faces from Google Fonts and keep the system fallbacks.
css = re.sub(r"@font-face \{[^}]*\}\n", "", css)
fonts = ('<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Plus+Jakarta+Sans:wght@500;600;700'
         '&family=IBM+Plex+Mono:wght@400;600&display=swap">')

html = html.replace('<link rel="stylesheet" href="assets/style.css">', fonts + "\n<style>\n" + css + "\n</style>")
html = html.replace('<script src="/static/schwung-remote-api.js"></script>', "")
html = html.replace('<script src="assets/params.js"></script>',
                    "<script>\n" + read("assets/params.js") + "\n</script>\n<script>\n" + read("dev/mock-bridge.js") + "\n</script>")
html = html.replace('<script src="assets/jp-model.js"></script>', "<script>\n" + read("assets/jp-model.js") + "\n</script>")
html = html.replace('<script src="assets/app.js"></script>', "<script>\n" + read("assets/app.js") + "\n</script>")
html = html.replace("<title>JE-8086</title>", "<title>JE-8086 Remote Panel</title>")
# Preview pages are wrapped by their host; drop our own document skeleton.
body = re.search(r"<head>(.*)</head>\s*<body>(.*)</body>", html, re.S)
html = body.group(1).replace('<meta charset="utf-8">', "").replace('<meta name="viewport" content="width=device-width, initial-scale=1">', "") + body.group(2)

os.makedirs(os.path.dirname(out), exist_ok=True)
open(out, "w").write(html.strip() + "\n")
print("wrote %s (%d KB)" % (os.path.relpath(out, ROOT), len(html) // 1024))
