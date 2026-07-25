# Lesson 1 — Binding Your First Type — Build

This folder is self-contained: the lesson doc, its source, and this build
guide all live together.

- Lesson doc: [01-binding-your-first-type.md](01-binding-your-first-type.md)
- Source: [lesson_01_binding.cpp](lesson_01_binding.cpp)

## Build

From the repository root, with the `debug` preset already configured
(`cmake --preset debug`, once per clone):

```powershell
cmake --build --preset debug --target employee_directory_lesson_01
```

## Run

```powershell
.\build\debug\employee_directory_lesson_01.exe
```

This is the first lesson, so running it starts the whole course fresh:
it (re)creates the persistent database file at
`docs/training/en/employee-directory.modb` and every later lesson opens
that SAME file and continues from there — see
[../README.md](../README.md#how-the-code-is-organized) for how the chain
works.
