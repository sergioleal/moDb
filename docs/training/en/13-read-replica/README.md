# Lesson 13 — A Read Replica for Reporting — Build

This folder is self-contained: the lesson doc, its source, and this build
guide all live together.

- Lesson doc: [13-read-replica.md](13-read-replica.md)
- Source: [lesson_13_read_replica.cpp](lesson_13_read_replica.cpp)

## Build

From the repository root, with the `debug` preset already configured
(`cmake --preset debug`, once per clone):

```powershell
cmake --build --preset debug --target employee_directory_lesson_13
```

## Run

```powershell
.\build\debug\employee_directory_lesson_13.exe
```

Running it replays every earlier lesson's code first (on one
continuously-reopened temp database file), then this lesson's own new
part — see [../README.md](../README.md#how-the-code-is-organized) for why.
