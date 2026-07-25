# Lesson 12 — Asynchronous WAL I/O — Build

This folder is self-contained: the lesson doc, its source, and this build
guide all live together.

- Lesson doc: [12-async-io.md](12-async-io.md)
- Source: [lesson_12_async_io.cpp](lesson_12_async_io.cpp)

## Build

From the repository root, with the `debug` preset already configured
(`cmake --preset debug`, once per clone):

```powershell
cmake --build --preset debug --target employee_directory_lesson_12
```

## Run

```powershell
.\build\debug\employee_directory_lesson_12.exe
```

Running it replays every earlier lesson's code first (on one
continuously-reopened temp database file), then this lesson's own new
part — see [../README.md](../README.md#how-the-code-is-organized) for why.
