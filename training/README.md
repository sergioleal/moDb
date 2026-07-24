# Ring0 Training

This directory contains the English training material for developers who already
know C++ and want to learn Ring0 incrementally through the phase examples.

Source material lives in `training/en`. Rendered HTML is generated into
`training/html`.

## Render HTML

From the repository root:

```powershell
py scripts/render_training.py
```

If Python is not on `PATH`, pass the interpreter explicitly through CMake:

```powershell
cmake -S . -B build/debug -DMODB_PYTHON_EXECUTABLE=<path-to-python>
cmake --build build/debug --target modb_training_html
```

Open `training/html/index.html` in a browser.
