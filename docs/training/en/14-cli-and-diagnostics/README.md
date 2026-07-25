# Lesson 14 -- CLI and Diagnostics -- Build

This folder is self-contained: the lesson doc, its source, and this build
guide all live together.

- Lesson doc: [14-cli-and-diagnostics.md](14-cli-and-diagnostics.md)
- Source: [lesson_14_cli_diagnostics.cpp](lesson_14_cli_diagnostics.cpp)

## Build

```powershell
cmake --build --preset debug --target employee_directory_lesson_14
```

## Run

```powershell
.\build\debug\employee_directory_lesson_14.exe
```

Run Lessons 1-13 first, in order. This lesson opens the persistent file they
built and inspects it from an operational point of view.
