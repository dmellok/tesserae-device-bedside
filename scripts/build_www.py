# Assemble www/index.html (served by the device) from www/app.src.html plus the
# Phosphor icon bodies in www/icons.js. Runs before every PlatformIO build.
import os
Import("env")  # noqa: F821
root = env.subst("$PROJECT_DIR")  # noqa: F821
src = os.path.join(root, "www", "app.src.html")
ico = os.path.join(root, "www", "icons.js")
out = os.path.join(root, "www", "index.html")
html = open(src, encoding="utf-8").read().replace("/*ICONS*/", open(ico, encoding="utf-8").read())
if not os.path.exists(out) or open(out, encoding="utf-8").read() != html:
    open(out, "w", encoding="utf-8").write(html)
    print("www: rebuilt index.html (%d bytes)" % len(html))

# .incbin has no dependency tracking and SCons compares contents, not times:
# stamp the page's hash into www.S so a changed page re-assembles it.
import hashlib, re
asm = os.path.join(root, "src", "www.S")
src_asm = open(asm, encoding="utf-8").read()
stamp = "/* www/index.html sha1 %s */" % hashlib.sha1(html.encode("utf-8")).hexdigest()
new_asm = re.sub(r"/\* www/index.html sha1 [0-9a-f]+ \*/\n?", "", src_asm).rstrip("\n") + "\n" + stamp + "\n"
if new_asm != src_asm:
    open(asm, "w", encoding="utf-8").write(new_asm)
