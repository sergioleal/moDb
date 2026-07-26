# Lesson 21 -- Load Testing and Dashboard -- Build

- Lesson doc: [21-load-testing-dashboard.md](21-load-testing-dashboard.md)
- Source: [lesson_21_load_testing_dashboard.cpp](lesson_21_load_testing_dashboard.cpp)

## Build

```powershell
cmake --build --preset debug --target modb_load employee_directory_lesson_21
```

## Run

```powershell
.\build\debug\employee_directory_lesson_21.exe
```

Run Lesson 20 first. This lesson is optional and operational: it teaches the
load-test workflow and dashboard, but it does not mutate the employee-directory
database.
