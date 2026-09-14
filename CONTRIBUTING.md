# Contributing

This is a recompilation project, which changes what a useful contribution looks
like. Two rules matter more than the rest.

## No application files, ever

Nothing off either disc goes in the repository, in any form: no sample, bitmap,
DLL, executable, layout table or saved arrangement, and nothing regenerated from
one - no lifted source, disassembly listing, reconstructed header or golden
output file with a lifted function in it.

The tool ships; the disc does not. Everyone brings their own copy and points the
tools at it. `.gitignore` covers `original/` and the generated trees; if you find
something that slips past it, that is a bug worth reporting on its own.

Screenshots of the thing running are fine and are the point of the README.

## Claims are measured, not assumed

Almost every bug in this project so far has been a number someone believed
without checking - a bar length taken from the wrong tempo, a panel rectangle
read off a screenshot, an argument assumed to be zero. A change that asserts
something about the engine or the file formats should come with the measurement
that says so, and where there is a corpus to check it against, a check in
`tools/conform.py`.

Run it before opening a pull request:

```
python tools/conform.py --root <your install>
```

It fails on regression against `tools/conform_baseline.json`, not on the eight
known exceptions. If your change moves the number legitimately, update the
baseline with `--update-baseline` and say why in the commit.

## The usual

- Imperative commit subjects, and the body explains *why* when it is not obvious.
- Feature branches for anything nontrivial; squash on merge.
- `flake8 tools --max-line-length 100` is what CI runs.
- Comments explain the reasoning, not the syntax. The engine is 25 years old and
  undocumented; a comment that records how something was found out is worth more
  than one that restates the line under it.
