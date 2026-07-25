# Lesson 4 — Handles and Updates — Build

This folder is self-contained: the lesson doc, its source, and this build
guide all live together.

- Lesson doc: [04-handles-and-updates.md](04-handles-and-updates.md)
- Source: [lesson_04_handles.cpp](lesson_04_handles.cpp)

## Build

From the repository root, with the `debug` preset already configured
(`cmake --preset debug`, once per clone):

```powershell
cmake --build --preset debug --target employee_directory_lesson_04
```

## Run

```powershell
.\build\debug\employee_directory_lesson_04.exe
```

Running it replays every earlier lesson's code first (on one
continuously-reopened temp database file), then this lesson's own new
part — see [../README.md](../README.md#how-the-code-is-organized) for why.
