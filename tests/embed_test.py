#!/usr/bin/env python3
"""Tests for tools/embed.py.

The generator's one job is to turn a file into a C string literal that holds
exactly the same bytes, split across several lines so that a diff can show a
change to it. Two things can go wrong and neither shows up in the build that
uses it, because the sources it is given today happen not to trigger them.

A cut can land in the middle of an escape sequence. The escaper writes a
backslash as two characters, and a cut between them leaves one fragment ending
in a lone backslash, which is not a string literal: the C compiler reads it as
the start of an escape and runs on into the next line looking for the
character it names. The generated header then does not compile, and the error
names a line of a generated file rather than the source that caused it.

A path can end a literal early. The table of file names is written as C
strings, and a name containing a quote would close one of them.

Both are checked here over every offset a cut could land at, because whether
they happen depends on where in the file the interesting byte sits, and
whether it happens for the sources the compiler ships today is a matter of
luck.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(os.path.dirname(HERE), "tools")
sys.path.insert(0, TOOLS)

import embed  # noqa: E402


def trailing_backslashes(s: str) -> int:
    n = 0
    for ch in reversed(s):
        if ch != "\\":
            break
        n += 1
    return n


def test_no_chunk_ends_mid_escape():
    """No fragment may end with an odd number of backslashes.

    An even run is a whole number of escaped backslashes. An odd one means
    the last of them is the first half of a pair whose other half is at the
    start of the next fragment, and each fragment is a literal of its own.
    """
    for pad in range(0, 260):
        for tail in (b"\\", b'"', b"\n", b"\x00", b"\xff"):
            data = b"A" * pad + tail + b"B" * 200
            chunks = embed.fragments(embed.c_bytes(data), embed.WIDTH)
            for chunk in chunks:
                if trailing_backslashes(chunk) % 2 == 1:
                    raise AssertionError(
                        "fragment ends with an odd run of backslashes at pad %d: %r"
                        % (pad, chunk[-8:])
                    )


def test_bytes_are_preserved():
    """The fragments must concatenate to exactly the escaped source."""
    for data in (b"", b"hello", b'a "quoted" thing', b"back\\slash", b"\x00\x01\xff",
                 b"line\nline\r\n", b"\\" * 40):
        pieces = embed.c_bytes(data)
        chunks = embed.fragments(pieces, embed.WIDTH)
        joined = "".join(chunks)
        assert joined == "".join(pieces), data
        # And the C compiler must read the same bytes back out of it.
        if not chunks:
            src = 'static const char x[] = "";\n'
        else:
            src = "static const char x[] =" + "".join(
                '\n    "%s"' % c for c in chunks
            ) + ";\n"
        src += "int main(void) { return sizeof(x) == %d ? 0 : 1; }\n" % (
            len(data) + 1
        )
        with tempfile.TemporaryDirectory() as d:
            c_path = os.path.join(d, "t.c")
            with open(c_path, "w") as f:
                f.write(src)
            r = subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-c", c_path,
                 "-o", os.path.join(d, "t.o")],
                capture_output=True,
                text=True,
            )
            assert r.returncode == 0, "generated literal does not compile:\n%s\n%s" % (
                src,
                r.stderr,
            )


def test_path_is_escaped():
    """A file name is data, and one with a quote in it must not end the literal."""
    with tempfile.TemporaryDirectory() as d:
        weird = os.path.join(d, 'a"b\\c.z')
        with open(weird, "w") as f:
            f.write("int main() { return 0; }\n")
        r = subprocess.run(
            [sys.executable, os.path.join(TOOLS, "embed.py"), "t", weird],
            capture_output=True,
            text=True,
        )
        assert r.returncode == 0, r.stderr
        out = r.stdout
        # The path is in the names table, escaped, so the quote is a \" and the
        # backslash is a \\.
        assert 'a\\"b\\\\c.z' in out, out
        # And the whole thing is a valid C file. The tables are unused here,
        # which is what the compiler says about them, so they are referenced.
        c_path = os.path.join(d, "gen.c")
        with open(c_path, "w") as f:
            f.write(out)
            f.write(
                "\nconst char *const *keep_names = t_names;\n"
                "const char *const *keep_srcs = t_srcs;\n"
                "int main(void) { return (keep_names && keep_srcs) ? 0 : 1; }\n"
            )
        c = subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-c", c_path,
             "-o", os.path.join(d, "gen.o")],
            capture_output=True,
            text=True,
        )
        assert c.returncode == 0, c.stderr


def main() -> int:
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_") or not callable(fn):
            continue
        try:
            fn()
            print("ok   embed %s" % name[5:])
        except Exception as exc:  # noqa: BLE001
            print("FAIL embed %s: %s" % (name[5:], exc), file=sys.stderr)
            failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
