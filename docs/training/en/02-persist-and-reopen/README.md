# Lesson 2 — Persist and Reopen — Build

This folder is self-contained: the lesson doc, its source, and this build
guide all live together.

- Lesson doc: [02-persist-and-reopen.md](02-persist-and-reopen.md)
- Source: [lesson_02_persist_reopen.cpp](lesson_02_persist_reopen.cpp)

## Build

From the repository root, with the `debug` preset already configured
(`cmake --preset debug`, once per clone):

```powershell
cmake --build --preset debug --target employee_directory_lesson_02
```

## Run

```powershell
.\build\debug\employee_directory_lesson_02.exe
```

This lesson only contains its own new code — it opens the persistent
database file Lesson 1 created and continues from there, so run Lesson 1
first. See [../README.md](../README.md#how-the-code-is-organized) for how
the chain works.
