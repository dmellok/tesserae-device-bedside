# Assemble the two pages the device serves, before every PlatformIO build:
#   www/index.html    the phone app, from www/app.src.html
#   www/desktop.html  the desktop app, from www/desktop.src.html
# Both get the Phosphor icon bodies (www/icons.js) in place of /*ICONS*/.
#
# The desktop page is a different shell (sidebar, pages, wide grids) around
# the SAME controls and script. It takes them from the phone source:
#   <!--@name-->   the element(s) after the marker <!--part:name--> (or
#                  <!--part:name n=3--> for several siblings) in app.src.html
#   /*@css*/       the phone page's stylesheet up to /*/css:shared*/
#   /*@js*/        the phone page's whole <script> body
# So a control exists once; each shell only decides where it goes.
import os, re, hashlib
Import("env")  # noqa: F821
root = env.subst("$PROJECT_DIR")  # noqa: F821
www = os.path.join(root, "www")
rd = lambda p: open(p, encoding="utf-8").read()
icons = rd(os.path.join(www, "icons.js"))
phone = rd(os.path.join(www, "app.src.html"))

def element_after(text, pos):
    """[start, end) of the first element starting at or after pos (comments and whitespace skipped)."""
    while True:
        m = re.compile(r"\s*(<!--.*?-->\s*)*", re.S).match(text, pos)
        pos = m.end()
        break
    m = re.compile(r"<([a-zA-Z][a-zA-Z0-9]*)").match(text, pos)
    if not m:
        raise SystemExit("build_www: no element after a part marker at %d" % pos)
    tag = m.group(1)
    depth, i = 0, pos
    opener = re.compile(r"<(/?)%s\b" % tag)
    while True:
        m2 = opener.search(text, i)
        if not m2:
            raise SystemExit("build_www: unbalanced <%s> in a part" % tag)
        depth += -1 if m2.group(1) else 1
        i = text.index(">", m2.end()) + 1
        if depth == 0:
            return pos, i

parts = {}
for m in re.finditer(r"<!--part:([a-z0-9_-]+)(?:\s+n=(\d+))?-->", phone):
    name, n, pos, chunks = m.group(1), int(m.group(2) or 1), m.end(), []
    for _ in range(n):
        a, b = element_after(phone, pos)
        chunks.append(phone[a:b]); pos = b
    parts[name] = "\n".join(chunks)

parts["favicon"] = re.search(r"<link rel=\"icon\"[^>]*>", phone).group(0)
css = re.search(r"<style>(.*?)/\*/css:shared\*/", phone, re.S).group(1)
js = re.search(r"<script>\s*(const ICONS=.*?)</script>\s*</body>", phone, re.S).group(1)   # the app script, not the head's redirect

def write_if_changed(path, html, label):
    if not os.path.exists(path) or rd(path) != html:
        open(path, "w", encoding="utf-8").write(html)
        print("www: rebuilt %s (%d bytes)" % (label, len(html)))

index_html = phone.replace("/*ICONS*/", icons)
write_if_changed(os.path.join(www, "index.html"), index_html, "index.html")

desk = rd(os.path.join(www, "desktop.src.html"))
def part(m):
    name = m.group(1)
    if name not in parts:
        raise SystemExit("build_www: desktop.src.html wants part '%s', not marked in app.src.html" % name)
    return parts[name]
desk = re.sub(r"<!--@([a-z0-9_-]+)-->", part, desk)
desk = desk.replace("/*@css*/", css).replace("/*@js*/", js).replace("/*ICONS*/", icons)
write_if_changed(os.path.join(www, "desktop.html"), desk, "desktop.html")

# .incbin has no dependency tracking and SCons compares contents, not times:
# stamp the pages' hashes into www.S so a changed page re-assembles it.
asm = os.path.join(root, "src", "www.S")
src_asm = rd(asm)
stamp = "/* www sha1 %s %s */" % (hashlib.sha1(index_html.encode("utf-8")).hexdigest(), hashlib.sha1(desk.encode("utf-8")).hexdigest())
new_asm = re.sub(r"/\* www(?:/index\.html)? sha1 [0-9a-f ]+ \*/\n?", "", src_asm).rstrip("\n") + "\n" + stamp + "\n"
if new_asm != src_asm:
    open(asm, "w", encoding="utf-8").write(new_asm)
