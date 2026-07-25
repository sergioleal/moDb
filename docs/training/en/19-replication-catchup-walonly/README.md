# Lesson 19 -- Replication Catch-up and wal_only -- Build

- Lesson doc: [19-replication-catchup-walonly.md](19-replication-catchup-walonly.md)
- Source: [lesson_19_replication_catchup_walonly.cpp](lesson_19_replication_catchup_walonly.cpp)

## Build

```powershell
cmake --build --preset debug --target employee_directory_lesson_19
```

## Run

```powershell
.\build\debug\employee_directory_lesson_19.exe
```

Run Lesson 18 first. This lesson is operational and does not mutate the
training database.
