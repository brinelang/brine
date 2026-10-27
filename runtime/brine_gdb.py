# gdb pretty-printers for brine programs. `brine debug` writes this file next
# to the debug build and loads it; by hand: `source brine-gdb.py` in gdb.
# Values print the way brine prints them: "hello", [1, 2, 3], (1, "two"),
# {"k": 9}, Shape.Rect(w=2, h=3), Some(5).
import gdb

# at most this many elements or bytes of a value are shown
LIMIT = 200


def _fmt(v):
    try:
        return v.format_string()
    except gdb.error:
        return "<invalid>"


def _ptr(v):
    try:
        p = int(v)
    except gdb.error:
        return None
    return p if p != 0 else None


class StrPrinter:
    def __init__(self, v):
        self.v = v

    def to_string(self):
        if _ptr(self.v) is None:
            return "<null>"
        try:
            s = self.v.dereference()
            n = int(s["len"])
            if n < 0 or n > 1 << 40:
                return "<invalid>"
            addr = int(s["data"].address)
            data = bytes(gdb.selected_inferior().read_memory(addr, min(n, LIMIT)))
        except (gdb.error, gdb.MemoryError):
            return "<invalid>"
        t = data.decode("utf-8", "replace")
        out = ['"']
        for c in t:
            if c == '"' or c == "\\":
                out.append("\\" + c)
            elif c == "\n":
                out.append("\\n")
            elif c == "\t":
                out.append("\\t")
            elif c == "\r":
                out.append("\\r")
            elif ord(c) < 32:
                out.append("\\x%02x" % ord(c))
            else:
                out.append(c)
        out.append('"')
        if n > LIMIT:
            out.append("...")
        return "".join(out)


def _items(p, n):
    k = min(n, LIMIT)
    out = [_fmt(p[i]) for i in range(k)]
    if n > k:
        out.append("...")
    return out


class ArrPrinter:
    def __init__(self, v):
        self.v = v

    def to_string(self):
        if _ptr(self.v) is None:
            return "<null>"
        try:
            a = self.v.dereference()
            n = int(a["len"])
            if n < 0 or n > int(a["cap"]):
                return "<invalid>"
            return "[" + ", ".join(_items(a["data"], n)) + "]"
        except (gdb.error, gdb.MemoryError):
            return "<invalid>"


class MapPrinter:
    def __init__(self, v):
        self.v = v

    def to_string(self):
        if _ptr(self.v) is None:
            return "<null>"
        try:
            m = self.v.dereference()
            used = int(m["used"])
            if used < 0 or used > int(m["cap"]):
                return "<invalid>"
            out = []
            for i in range(used):
                if len(out) == LIMIT:
                    out.append("...")
                    break
                if bool(m["live"][i]):
                    out.append(_fmt(m["keys"][i]) + ": " + _fmt(m["vals"][i]))
            return "{" + ", ".join(out) + "}"
        except (gdb.error, gdb.MemoryError):
            return "<invalid>"


class TuplePrinter:
    def __init__(self, v):
        self.v = v

    def to_string(self):
        fs = self.v.type.strip_typedefs().fields()
        out = [_fmt(self.v[f.name]) for f in fs]
        if len(out) == 1:
            return "(" + out[0] + ",)"
        return "(" + ", ".join(out) + ")"


class BoxPrinter:
    def __init__(self, v):
        self.v = v

    def to_string(self):
        if _ptr(self.v) is None:
            return "<null>"
        try:
            b = self.v.dereference()
            label = _fmt(b["tag"])
            name = label.rsplit(".", 1)[-1]
            u = b["variant"]
            if name not in [f.name for f in u.type.strip_typedefs().fields()]:
                return label
            var = u[name]
            fs = var.type.strip_typedefs().fields()
            if "." in label:
                args = [f.name + "=" + _fmt(var[f.name]) for f in fs]
            else:
                # Option and Result print their payload alone
                args = [_fmt(var[f.name]) for f in fs]
            return label + "(" + ", ".join(args) + ")"
        except (gdb.error, gdb.MemoryError):
            return "<invalid>"


class BytePrinter:
    def __init__(self, v):
        self.v = v

    def to_string(self):
        return str(int(self.v))


def lookup(v):
    t = v.type
    name = t.name
    if name is None:
        return None
    if name == "str":
        return StrPrinter(v)
    if name in ("u8", "i8"):
        return BytePrinter(v)
    if name.startswith("["):
        return ArrPrinter(v)
    if name.startswith("{"):
        return MapPrinter(v)
    if name.startswith("("):
        return TuplePrinter(v)
    t = t.strip_typedefs()
    if t.code == gdb.TYPE_CODE_PTR:
        target = t.target()
        if target.tag is not None and target.tag.startswith("brine_box "):
            return BoxPrinter(v)
    return None


def _setup():
    gdb.pretty_printers.append(lookup)
    # don't step into the C runtime
    for f in ("brine_rt.c", "brine_rt.h"):
        gdb.execute("skip -gfi */" + f, to_string=True)


_setup()
