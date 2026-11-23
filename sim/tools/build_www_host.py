# Run scripts/build_www.py (a PlatformIO pre-build script) outside PlatformIO:
# it only needs Import("env") and env.subst("$PROJECT_DIR").
import os, sys
root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
class Env:
    def subst(self, s): return s.replace("$PROJECT_DIR", root)
script = os.path.join(root, "scripts", "build_www.py")
exec(compile(open(script).read(), script, "exec"), {"Import": lambda *a: None, "env": Env(), "__file__": script})
