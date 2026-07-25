# Lesson 5 — Relationships: Departments and Projects — Build

This folder is self-contained: the lesson doc, its source, and this build
guide all live together.

- Lesson doc: [05-relationships.md](05-relationships.md)
- Source: [lesson_05_relationships.cpp](lesson_05_relationships.cpp)

## Build

From the repository root, with the `debug` preset already configured
(`cmake --preset debug`, once per clone):

```powershell
cmake --build --preset debug --target employee_directory_lesson_05
```

## Run

```powershell
.\build\debug\employee_directory_lesson_05.exe
```

This lesson only contains its own new code — it opens the persistent
database file Lessons 1-4 built up and continues from there, so run
Lessons 1-4 first, in order. See
[../README.md](../README.md#how-the-code-is-organized) for how the chain
works.
