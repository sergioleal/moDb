# Lesson 6 — Consistent Reports with Snapshots — Build

This folder is self-contained: the lesson doc, its source, and this build
guide all live together.

- Lesson doc: [06-snapshots.md](06-snapshots.md)
- Source: [lesson_06_snapshots.cpp](lesson_06_snapshots.cpp)

## Build

From the repository root, with the `debug` preset already configured
(`cmake --preset debug`, once per clone):

```powershell
cmake --build --preset debug --target employee_directory_lesson_06
```

## Run

```powershell
.\build\debug\employee_directory_lesson_06.exe
```

This lesson only contains its own new code — it opens the persistent
database file Lessons 1-5 built up and continues from there, so run
Lessons 1-5 first, in order. See
[../README.md](../README.md#how-the-code-is-organized) for how the chain
works.
